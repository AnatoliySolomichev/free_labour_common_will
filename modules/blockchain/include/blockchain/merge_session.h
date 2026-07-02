#pragma once

#include "storage.h"
#include "validator.h"

namespace blockchain {

// Two-round bilateral merge protocol (§6.4).
//
// Typical call order:
//
//   Alice                               Bob
//   prepare_tip()  ──────────────────►  verify_partner_tip()
//   verify_partner_tip()  ◄────────────  prepare_tip()
//   create_pending() ─── draft_hash ───► co_sign()
//   co_sign()       ◄── draft_hash ────  create_pending()
//   finalize()      ◄──── co_sig ──────  finalize()
//                   ─────── co_sig ────►
class MergeSession {
public:
    MergeSession(IStorage& storage, Validator& validator);

    // Step 1a: build BranchTipInfo for own branch to send to partner.
    // Throws: NodeNotFoundError, BlockNotFoundError.
    BranchTipInfo prepare_tip(const UserId& user_id, NodeIndex leaf_index) const;

    // Step 1b: verify partner's BranchTipInfo (path signatures + chain).
    // Throws: SignatureError, ChainIntegrityError, NodeNotFoundError.
    void verify_partner_tip(const BranchTipInfo& partner_tip) const;

    // Current accumulated snapshot of a branch (blockchain.md §6.5.1): the stored
    // snapshot if the branch has merged before, otherwise a fresh single-leaf
    // snapshot over the current tip. Send this to the partner during a merge.
    // Throws: InvalidArgumentError (empty branch), StorageError, BlockNotFoundError.
    MergeSnapshot snapshot_for(const UserId& user_id, NodeIndex leaf_index) const;

    // Step 2: create a MERGE block draft (own signature; co_signature absent).
    // Unions the own snapshot with the partner's, commits the resulting
    // merkle_root + hll_hash into the block, and retains the grown snapshot for
    // the branch. validated_depth is the author's self-declared verification
    // depth (§6.5.5). The draft is saved to storage immediately (without
    // co_signature); call finalize() once the partner's co-signature arrives.
    // Throws: CryptoError, StorageError, NodeNotFoundError,
    //         InvalidArgumentError (own or partner branch empty, §6.4).
    PendingMergeBlock create_pending(
        const UserId&        user_id,
        NodeIndex            leaf_index,
        const BranchTipInfo& partner_tip,
        const MergeSnapshot& partner_snapshot,
        const KeyPair&       own_working_keypair,
        Timestamp            merge_timestamp,
        uint32_t             validated_depth
    );

    // Step 3: co-sign the partner's draft hash with own key.
    // Returns the Signature to send back to the partner.
    // Throws: CryptoError.
    Signature co_sign(const Hash& partner_draft_hash, const KeyPair& own_working_keypair);

    // Step 4: attach partner's co_signature to own pending block and finalize.
    // Verifies co_sig, stores it as a Seal (SealMode::OPEN) in the seals table,
    // and returns the block with co_signature set in memory.
    // Throws: SignatureError (invalid co_sig), StorageError.
    Block finalize(const PendingMergeBlock& pending,
                   const Signature&         partner_co_sig,
                   const PublicKey&         partner_pubkey);

    // Step 5 (optional): import partner's path nodes and tip block into local storage.
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
