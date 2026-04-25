#pragma once

// Convenience umbrella header — include this to get the full public API.

#include "types.h"
#include "errors.h"
#include "crypto.h"
#include "serializer.h"
#include "storage.h"
#include "validator.h"
#include "seal_manager.h"
#include "merge_session.h"

#include <functional>
#include <vector>

namespace blockchain {

// Main facade. Delegates to IStorage and Validator.
// storage and validator must outlive Blockchain.
class Blockchain {
public:
    Blockchain(IStorage& storage, Validator& validator);

    // ── Identity (§6.1) ───────────────────────────────────────────────────────

    // Create root node (index 0) with self-signature and store it.
    // Throws: CryptoError, StorageError, InvalidArgumentError (already exists).
    Node create_identity(const KeyPair& root_keypair);

    // ── Node tree (§6.2) ──────────────────────────────────────────────────────

    // Ensure every node on the path from root to leaf_index exists.
    // For each missing node, calls key_for(node_index) to obtain a KeyPair,
    // then creates and stores the node.
    // Returns only the newly created nodes in root→leaf order.
    // Throws: CryptoError, StorageError, NodeNotFoundError (root absent),
    //         InvalidArgumentError (leaf_index is not a leaf, i.e. depth != 32).
    std::vector<Node> ensure_path(
        const UserId&                      user_id,
        NodeIndex                          leaf_index,
        std::function<KeyPair(NodeIndex)>  key_for
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

    // Append a KEY_ROTATION block (§6.6). Signs with old_working_keypair.
    // Throws: CryptoError, StorageError, NodeNotFoundError.
    Block rotate_key(
        const UserId&  user_id,
        NodeIndex      leaf_index,
        const KeyPair& old_working_keypair,
        const KeyPair& new_keypair,
        Timestamp      timestamp
    );

    // Append a REVOCATION block (§6.7). Signs with parent_keypair.
    // [OPEN §11.1] placement of the block is an open question; currently appended
    // to the parent node's service branch.
    // Throws: CryptoError, StorageError, NodeNotFoundError.
    Block revoke_node(
        const UserId&    user_id,
        NodeIndex        revoked_node_index,
        Timestamp        compromised_since,
        const PublicKey& replacement_pubkey,
        const KeyPair&   parent_keypair,
        Timestamp        timestamp
    );

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
    IStorage&  storage_;
    Validator& validator_;
};

} // namespace blockchain
