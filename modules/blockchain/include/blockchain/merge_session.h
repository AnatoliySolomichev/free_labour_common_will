#pragma once

#include "storage.h"
#include "validator.h"

namespace blockchain {

// Bilateral merge protocol (blockchain.md §6.4): each side's signature stands in
// its own branch. The initiator writes its half first; the responder reads it
// and answers with its half, which takes the initiator's half itself.
//
// Typical call order:
//
//   Alice (initiator)                     Bob (responder)
//   prepare_tip()  ──── tip, snapshot ──►  verify_partner_tip()
//   verify_partner_tip()  ◄── tip, snapshot ── prepare_tip()
//   write_half()   ────── her half ─────►  check_partner_half()
//                                          write_half(tip_after(her tip, her half))
//   check_partner_half() ◄── his half ───
class MergeSession {
public:
    MergeSession(IStorage& storage, Validator& validator);

    // Step 1a: build BranchTipInfo for own branch to send to partner.
    // Throws: NodeNotFoundError, BlockNotFoundError.
    BranchTipInfo prepare_tip(const UserId& user_id, NodeIndex leaf_index) const;

    // Step 1b: verify partner's BranchTipInfo (path signatures + chain).
    // Throws: SignatureError, ChainIntegrityError, NodeNotFoundError.
    void verify_partner_tip(const BranchTipInfo& partner_tip) const;

    // Step 1c: the partner's tip is not from the future (blockchain.md §9.4): a
    // branch dated later than `now` by more than kClockSlack is not merged with —
    // it waits until time catches up with it.
    // Throws: TimestampError.
    void check_partner_time(const BranchTipInfo& partner_tip, Timestamp now) const;

    // Current accumulated snapshot of a branch (blockchain.md §6.5.1): the stored
    // snapshot if the branch has merged before, otherwise a fresh single-leaf
    // snapshot over the current tip. Send this to the partner during a merge.
    // Throws: InvalidArgumentError (empty branch), StorageError, BlockNotFoundError.
    MergeSnapshot snapshot_for(const UserId& user_id, NodeIndex leaf_index) const;

    // Whether this branch may open a bilateral act now (blockchain.md §6.7): a
    // revoked branch is frozen, or goes on only under the key the revocation
    // named. Mirrors Blockchain::ensure_branch_writable; merges bypass the facade.
    // Throws: RevocationError.
    void check_own_branch(const UserId& user_id, NodeIndex leaf_index,
                          const KeyPair& own_working_keypair) const;

    // Step 2: write own half: a MERGE block right after `own_tip` — the tip sent
    // to the partner — that takes `partner_tip`. Unions the own snapshot with the
    // partner's, commits the resulting merkle_root + hll_hash into the block, and
    // retains the grown snapshot for the branch. validated_depth is the author's
    // self-declared verification depth (blockchain.md §6.5.5). The block is signed
    // and in the branch on return: nothing is added to it later. An unanswered
    // half stays an ordinary block that fixes nothing (blockchain.md §5.2).
    // Throws: CryptoError, StorageError, NodeNotFoundError, RevocationError,
    //         ChainIntegrityError (own branch moved past `own_tip`),
    //         InvalidArgumentError (own or partner branch empty, blockchain.md §6.4).
    Block write_half(const BranchTipInfo& own_tip,
                     const BranchTipInfo& partner_tip,
                     const MergeSnapshot& partner_snapshot,
                     const KeyPair&       own_working_keypair,
                     Timestamp            merge_timestamp,
                     uint32_t             validated_depth);

    // Step 3: read the partner's half before answering it, or before taking the
    // answer (blockchain.md §6.4): a MERGE block right after `partner_tip` that
    // takes `own_tip`, at a time within kClockSlack of our `now`, committing
    // `merged` — the union of both pre-merge snapshots. The responder's own_tip is
    // its tip; the initiator's is tip_after(own tip, own half). The half's signature
    // is not checked here: whoever counts it does — the aggregator's door, the
    // reader — and a side would need all the partner's key changes for it.
    // Throws: ChainIntegrityError, TimestampError, SerializationError,
    //         InvalidArgumentError (empty partner branch).
    void check_partner_half(const Block& half, const BranchTipInfo& own_tip,
                            const BranchTipInfo& partner_tip, Timestamp now,
                            const MergeSnapshot& merged) const;

    // `tip` moved on to `half`: what the responder's half takes, and what the
    // initiator reads the answer against.
    static BranchTipInfo tip_after(const BranchTipInfo& tip, const Block& half);

    // Step 4 (optional): import partner's path nodes and tip block into local storage.
    // Path nodes are stored in the nodes table under the partner's user_id.
    // The tip block (if present in partner_tip) is stored in external_blocks.
    // Safe to call multiple times with the same partner (idempotent).
    // Throws: StorageError.
    void import_partner_data(const BranchTipInfo& partner_tip);

private:
    IStorage&  storage_;
    Validator& validator_;
};

} // namespace blockchain
