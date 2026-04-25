#pragma once

#include "storage.h"

namespace blockchain {

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
