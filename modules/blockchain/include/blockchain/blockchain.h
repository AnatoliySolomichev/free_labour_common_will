#pragma once

// Convenience umbrella header — include this to get the full public API.

#include "types.h"
#include "errors.h"
#include "crypto.h"
#include "serializer.h"
#include "storage.h"
#include "validator.h"
#include "revocation.h"
#include "merge_session.h"

#include <functional>
#include <vector>

namespace blockchain {

// Main facade. Delegates to IStorage and Validator.
// storage and validator must outlive Blockchain.
class Blockchain {
public:
    Blockchain(IStorage& storage, Validator& validator);

    // ── Identity (blockchain.md §6.1) ───────────────────────────────────────────────────────

    // Create root node (index 0) with self-signature and store it.
    // Throws: CryptoError, StorageError, InvalidArgumentError (already exists).
    Node create_identity(const KeyPair& root_keypair);

    // ── Node tree (blockchain.md §6.2) ──────────────────────────────────────────────────────

    // Ensure every node on the path from root to leaf_index exists.
    // For each missing node, calls key_for(node_index) to obtain a KeyPair,
    // then creates and stores the node.
    // Returns only the newly created nodes in root→leaf order.
    // Throws: CryptoError, StorageError, NodeNotFoundError (root absent),
    //         InvalidArgumentError (leaf_index is not a leaf, i.e. depth != 31).
    std::vector<Node> ensure_path(
        const UserId&                      user_id,
        NodeIndex                          leaf_index,
        std::function<KeyPair(NodeIndex)>  key_for,
        Timestamp                          timestamp = 0
    );

    // Throws: NodeNotFoundError.
    Node get_node(const UserId& user_id, NodeIndex index) const;

    // All nodes from root to leaf_index from storage.
    // Throws: NodeNotFoundError (if any node on the path is absent).
    std::vector<Node> get_path(const UserId& user_id, NodeIndex leaf_index) const;

    // ── Branch operations ─────────────────────────────────────────────────────

    // Hash of branch tip: hash(last block) or hash(leaf node) if branch is empty.
    // Throws: NodeNotFoundError.
    Hash branch_tip_hash(const UserId& user_id, NodeIndex leaf_index) const;

    // Append a DATA block. Determines prev_hash automatically.
    // Throws: CryptoError, StorageError, NodeNotFoundError,
    //         InvalidArgumentError (leaf_index not a leaf).
    Block append_data_block(
        const UserId&        user_id,
        NodeIndex            leaf_index,
        std::vector<uint8_t> payload,
        const KeyPair&       working_keypair,
        Timestamp            timestamp
    );

    // Append a "stub" DATA block: empty CBOR array payload, no user records.
    // Two uses (blockchain.md §5.4): (1) bootstrap an empty branch so it can take part in a
    // merge (blockchain.md §6.4 requires a block-0); (2) create a fresh tip to anchor time after
    // a long idle period — once witnessed via merge, subsequent work is provably
    // later.
    // Throws: CryptoError, StorageError, NodeNotFoundError, InvalidArgumentError.
    Block append_stub_block(
        const UserId&  user_id,
        NodeIndex      leaf_index,
        const KeyPair& working_keypair,
        Timestamp      timestamp
    );

    // Append a KEY_ROTATION block (blockchain.md §6.6). Signs with old_working_keypair.
    // Throws: CryptoError, StorageError, NodeNotFoundError.
    Block rotate_key(
        const UserId&  user_id,
        NodeIndex      leaf_index,
        const KeyPair& old_working_keypair,
        const KeyPair& new_keypair,
        Timestamp      timestamp
    );

    // Append a REVOCATION block (blockchain.md §6.7) to the branch of ancestor_index.
    // ancestor_index must be a strict ancestor of revoked_node_index; the default
    // radius is the parent (N-1)/2, higher ancestors — by choice or escalation.
    // replacement_pubkey == nullopt is the emergency stop; a replacement may be
    // assigned later by a second REVOCATION (blockchain.md §6.7 rule 3, "последнее слово").
    // revoked_pubkey is filled from the revoked branch's current effective
    // working key (node key + KEY_ROTATION history, or the replacement in force).
    // last_good names the owner's last valid block of the revoked branch
    // (blockchain.md §6.7 rule 12) — it must be a block of that branch in this
    // storage; without an own copy restore_branch brings the line first.
    // Throws: CryptoError, StorageError, NodeNotFoundError,
    //         RevocationError (not an ancestor / root / self),
    //         InvalidArgumentError (ancestor keypair mismatch, unknown last_good).
    Block revoke_node(
        const UserId&                   user_id,
        NodeIndex                       revoked_node_index,
        const std::optional<PublicKey>& replacement_pubkey,
        NodeIndex                       ancestor_index,
        const KeyPair&                  ancestor_keypair,
        Timestamp                       timestamp,
        const std::optional<Hash>&      last_good = std::nullopt
    );

    // Own blocks of a branch learned elsewhere — the aggregator, a partner — for
    // the owner without a copy of it (blockchain.md §6.7 rule 12): the line up to
    // the block the owner recognizes as the last valid one, so that a revocation
    // names it from this storage and a replacement continues right after it.
    // `blocks` follow this storage's copy of the branch in order (from #0 for an
    // empty one); all of them pass Validator::check_continuation before any is
    // stored. Throws: ChainIntegrityError, TimestampError, SignatureError,
    // NodeNotFoundError, StorageError.
    void restore_branch(const UserId& user_id, NodeIndex node_index,
                        const std::vector<Block>& blocks);

    // Current effective working key of a branch: the node's working_pubkey,
    // advanced by KEY_ROTATION blocks in the branch (blockchain.md §6.6).
    // Throws: NodeNotFoundError, BlockNotFoundError.
    PublicKey effective_working_pubkey(const UserId& user_id, NodeIndex node_index) const;

    // ── Reading ───────────────────────────────────────────────────────────────

    // Throws: BlockNotFoundError.
    Block get_block(const BlockAddress& address) const;

    // All blocks in a branch from index 0 to tip, in ascending order.
    // Throws: NodeNotFoundError, BlockNotFoundError.
    std::vector<Block> get_branch(const UserId& user_id, NodeIndex leaf_index) const;

    // ── Validation ────────────────────────────────────────────────────────────

    // Throws: SignatureError, NodeNotFoundError.
    void validate_path(const UserId& user_id, NodeIndex leaf_index) const;

    // Throws: SignatureError, ChainIntegrityError, TimestampError,
    //         BlockNotFoundError, NodeNotFoundError.
    void validate_branch(const UserId& user_id, NodeIndex leaf_index) const;

private:
    // blockchain.md §6.7: refuse to extend a FROZEN branch, or a REPLACED one with a key other
    // than the authorized replacement lineage. ACTIVE branches are not checked
    // here (invariant 7 is validation's job).
    void ensure_branch_writable(const UserId& user_id, NodeIndex node_index,
                                const PublicKey& signing_pubkey) const;

    IStorage&  storage_;
    Validator& validator_;
};

} // namespace blockchain
