#pragma once

#include "storage.h"

namespace blockchain {

// Effective revocation state of a node, resolved over all REVOCATION blocks
// found in its ancestors' branches (blockchain.md §6.7): the highest ancestor wins (blockchain.md §4.4);
// within the winning branch the latest block has the last word on the
// replacement and on the line ("stop now, name the line and replace later").
// A summary over all revoked keys of the node; the branch walk
// (branch_revocation_status) reads the records key by key.
struct RevocationState {
    std::optional<PublicKey> replacement_pubkey; // nullopt = branch is frozen
    std::optional<LastGood>  last_good;          // the latest named last valid block (rule 12)
    BlockAddress             latest;             // block that had the last word
};

// Branch-level effect of the effective revocation (blockchain.md §6.7 rules 3, 9).
enum class BranchRevocationState : uint8_t {
    ACTIVE,    // no effective revocation
    FROZEN,    // revoked, no replacement assigned — no key may extend the branch
    REPLACED,  // revoked with a replacement key
};

// Per-block classification relative to the revocation zones (blockchain.md §6.7 rules 5, 9, 12).
// Structure ≠ weight: SUSPECT and OUTSIDE_LINE blocks are structurally valid —
// the economic layer (wallet, merge acceptance) weighs them by record kind.
// Keys come in epochs: the node key with its rotations, then each replacement
// with its own; a revocation of any key of an epoch covers the whole epoch.
enum class BlockRevocationStatus : uint8_t {
    CLEAN,
    SUSPECT,                    // signed by a revoked key, no line named yet: in question
    INVALID_AFTER_REPLACEMENT,  // signed by the old key after the replacement took over
    OUTSIDE_LINE,               // signed by the revoked key past the owner's last valid block
};

struct BranchRevocationStatus {
    BranchRevocationState               state;
    std::optional<RevocationState>      revocation; // effective record, if any
    std::vector<BlockRevocationStatus>  blocks;     // aligned with block indices
    // Key authorized to sign the next block: the rotation-lineage tip for
    // ACTIVE, the replacement (or its rotated successor) for REPLACED,
    // nullopt for FROZEN.
    std::optional<PublicKey>            next_key;
    // Lineage tip of the latest key epoch the branch reached — the key a new
    // revocation names when no other key is authorized yet.
    PublicKey                           current_key{};
    // Index of the owner's last valid block of the latest revoked epoch, when
    // a revocation named it and the block is in the branch (rule 12).
    std::optional<BlockIndex>           line_end;
};

// Verify a block's signature against a candidate key (canonical CBOR bytes
// with the signature zeroed).
bool block_signed_by(const Block& block, const PublicKey& key);

// Checks invariants (blockchain.md §9). Read-only access to storage; no state mutation.
class Validator {
public:
    explicit Validator(const IStorage& storage);

    // ── Node checks (invariants 1, 3, 6) ─────────────────────────────────────

    // The root: its record is signed by itself. Any other node: its birth block
    // (own or imported) is the one it names, says what the node says, and is
    // signed by the parent's key in force at its place (blockchain.md §11.2).
    // Throws: SignatureError, ChainIntegrityError, NodeNotFoundError, BlockNotFoundError.
    void validate_node(const Node& node, const UserId& user_id) const;

    // Verify the full path from root to leaf_index.
    // Throws: SignatureError, NodeNotFoundError, InvariantError.
    void validate_path(const UserId& user_id, NodeIndex leaf_index) const;

    // ── Branch checks (invariants 2, 3, 4, 5, 7) ─────────────────────────────

    // Verify a single block: prev_hash, signature, timestamp monotonicity.
    // Throws: SignatureError, ChainIntegrityError, TimestampError.
    void validate_block(const Block& block,
                        const Hash& expected_prev_hash,
                        const PublicKey& expected_working_pubkey) const;

    // Verify the full branch from block 0 to tip (tracks KEY_ROTATION).
    // Throws: SignatureError, ChainIntegrityError, TimestampError,
    //         BlockNotFoundError, NodeNotFoundError.
    void validate_branch(const UserId& user_id, NodeIndex leaf_index) const;

    // ── Revocation checks (blockchain.md §6.7) ──────────────────────────────────────────────

    // Semantic validation of a REVOCATION block stored in an ancestor branch:
    // payload decodes, the author node is a strict ancestor of the revoked node,
    // the revoked node is not the root and exists, revoked_pubkey matches one of
    // its historical working keys (node key, KEY_ROTATION keys, or a replacement
    // assigned by an earlier revocation). Signature/prev_hash integrity is the
    // job of validate_branch (invariant 7).
    // Throws: InvalidArgumentError (not a REVOCATION block), SerializationError,
    //         NodeNotFoundError, RevocationError.
    void validate_revocation(const Block& block) const;

    // Resolve the effective revocation state of a node over the revocation index.
    // Records whose author was itself under revocation at writing time are
    // ignored (blockchain.md §6.7 rule 10, recursive author-validity filter). nullopt = not
    // revoked.
    std::optional<RevocationState> effective_revocation(
        const UserId& user_id, NodeIndex node_index) const;

    // Classify a branch and each of its blocks against the effective revocation
    // (blockchain.md §6.7 rules 5, 9, 11, 12): follows the key epochs — the
    // first block of a replacement switches the key, over replacements revoked
    // before any use; within a revoked epoch the owner's line (up to the named
    // last valid block) is CLEAN and the rest OUTSIDE_LINE; with no line
    // named its blocks are SUSPECT. Throws: NodeNotFoundError, BlockNotFoundError,
    // SignatureError (block signed by none of the branch keys — structurally
    // broken branch).
    BranchRevocationStatus branch_revocation_status(
        const UserId& user_id, NodeIndex node_index) const;

    // The key a branch signs with at `place` (blockchain.md §6.6, blockchain.md §6.7 rule 9):
    // its node's key carried through the key changes of the stored blocks
    // before that place. Throws: NodeNotFoundError, SignatureError.
    PublicKey key_at(const UserId& user_id, NodeIndex node_index, BlockIndex place) const;

    // Whether `more` continues the branch as this storage holds it, before
    // anything is stored — blocks of an own branch learned elsewhere, by the
    // owner without a copy of it (blockchain.md §6.7 rule 12): consecutive
    // numbers from the next one, prev_hash links from the stored tip (from the
    // node for an empty branch), no date before the previous one or before the
    // birth (blockchain.md §11.2), each block signed by the branch's key at its
    // place — the key epochs as branch_revocation_status follows them. Under a
    // revocation the blocks may be in question or off a line: structure ≠ weight
    // (rule 11). Throws: ChainIntegrityError, TimestampError, SignatureError,
    // NodeNotFoundError.
    void check_continuation(const UserId& user_id, NodeIndex node_index,
                            const std::vector<Block>& more) const;

    // Whether a birth stands under a revocation of the parent (rule 6): on the
    // owner's line it names; with no line named nothing stands yet.
    bool birth_stands(const Block& birth, const RevocationState& st) const;

    // blockchain.md §6.7 rule 6: true if any edge on the path root→node was created under a
    // revoked parent key: its birth lies off the owner's line.
    // Throws: NodeNotFoundError.
    bool node_invalidated_by_revocation(
        const UserId& user_id, NodeIndex node_index) const;

    // blockchain.md §6.7 rule 11 — the acceptance-time check: match a partner's branch tip
    // against local revocation knowledge (own records + imported certificates).
    // Uses the nodes carried by the tip itself — the partner's tree need not be
    // in storage. Refuses: a frozen branch; a tip of a replaced branch not
    // signed by the replacement key (stale or stolen; a rotation *after* the
    // replacement is not recognized without the partner's branch history — MVP
    // limitation); a path node whose birth does not stand (fake subtree, rule 6). No local knowledge — no
    // objection: freshness is the caller's policy (sync.md §10.3).
    // Throws: RevocationError.
    void check_tip_against_revocations(const BranchTipInfo& tip) const;

private:
    // REVOCATION records targeting `target`, from the storage index, filtered by
    // ancestry and author validity (blockchain.md §6.7 rule 10), ordered root-most first, then
    // by block index.
    std::vector<std::pair<BlockAddress, RevocationPayload>> collect_revocations(
        const UserId& user_id, NodeIndex target) const;

    // The records of the winning (highest) ancestor, in its branch order.
    std::vector<std::pair<BlockAddress, RevocationPayload>> winning_records(
        const UserId& user_id, NodeIndex target) const;

    // blockchain.md §6.7 rules 10, 12: does a record of an author that is itself under
    // revocation count? Stored author branch — only if the record is CLEAN in
    // it (the author's own line); an imported certificate without the branch —
    // by time, or when signed by the author's replacement.
    bool author_record_counts(const Block& record, bool own_branch,
                              const RevocationState& author_state) const;

    // branch_revocation_status over the stored branch followed by `more`.
    BranchRevocationStatus status_over(const UserId& user_id, NodeIndex node_index,
                                       const std::vector<Block>& more) const;

    const IStorage& storage_;
};

} // namespace blockchain
