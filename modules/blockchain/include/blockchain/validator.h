#pragma once

#include "storage.h"

namespace blockchain {

// Effective revocation state of a node, resolved over all REVOCATION blocks
// found in its ancestors' branches (§6.7): the highest ancestor wins (§4.4);
// within the winning branch, the earliest compromised_since applies and the
// latest block has the last word on replacement ("stop now, replace later").
struct RevocationState {
    Timestamp                compromised_since;
    std::optional<PublicKey> replacement_pubkey; // nullopt = branch is frozen
    BlockAddress             latest;             // block that had the last word
};

// Checks invariants (§9). Read-only access to storage; no state mutation.
class Validator {
public:
    explicit Validator(const IStorage& storage);

    // ── Node checks (invariants 1, 3, 6) ─────────────────────────────────────

    // Verify node signature against parent key (or self-signature for root).
    // Throws: SignatureError, NodeNotFoundError (if parent absent).
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

    // ── Revocation checks (§6.7) ──────────────────────────────────────────────

    // Semantic validation of a REVOCATION block stored in an ancestor branch:
    // payload decodes, the author node is a strict ancestor of the revoked node,
    // the revoked node is not the root and exists, revoked_pubkey matches one of
    // its historical working keys (node key, KEY_ROTATION keys, or a replacement
    // assigned by an earlier revocation). Signature/prev_hash integrity is the
    // job of validate_branch (invariant 7).
    // Throws: InvalidArgumentError (not a REVOCATION block), SerializationError,
    //         NodeNotFoundError, RevocationError.
    void validate_revocation(const Block& block) const;

    // Resolve the effective revocation state of a node by scanning the branches
    // of all its ancestors present in storage. nullopt = not revoked.
    std::optional<RevocationState> effective_revocation(
        const UserId& user_id, NodeIndex node_index) const;

    // ── Seal check (invariant 3) ──────────────────────────────────────────────

    // Throws: SignatureError.
    void validate_seal(const Seal& seal) const;

    // ── MERGE block co-signature check ────────────────────────────────────────

    // Verify co_signature is a valid signature of partner_pubkey on
    // Crypto::hash_block(block).
    // Throws: SignatureError, InvalidArgumentError (if block.type != MERGE).
    void validate_co_signature(const Block& block, const PublicKey& partner_pubkey) const;

private:
    const IStorage& storage_;
};

} // namespace blockchain
