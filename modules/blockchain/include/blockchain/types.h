#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

namespace blockchain {

// ── Primitive crypto types ────────────────────────────────────────────────────

struct PublicKey {
    std::array<uint8_t, 32> bytes{};

    bool operator==(const PublicKey& o) const noexcept { return bytes == o.bytes; }
    bool operator!=(const PublicKey& o) const noexcept { return bytes != o.bytes; }
    bool operator< (const PublicKey& o) const noexcept { return bytes <  o.bytes; }
};

struct SecretKey {
    std::array<uint8_t, 64> bytes{};
};

struct KeyPair {
    PublicKey pub;
    SecretKey sec;
};

struct Hash {
    std::array<uint8_t, 32> bytes{};

    bool operator==(const Hash& o) const noexcept { return bytes == o.bytes; }
    bool operator!=(const Hash& o) const noexcept { return bytes != o.bytes; }

    static Hash zero() noexcept { return Hash{}; }
};

struct Signature {
    std::array<uint8_t, 64> bytes{};

    bool operator==(const Signature& o) const noexcept { return bytes == o.bytes; }
    bool operator!=(const Signature& o) const noexcept { return bytes != o.bytes; }

    static Signature null() noexcept { return Signature{}; }
};

// ── Type aliases ──────────────────────────────────────────────────────────────

using UserId     = PublicKey;   // root node public key
using NodeIndex  = uint32_t;    // heap index in the node tree
using BlockIndex = uint32_t;    // sequential index of a block in a branch
using Timestamp  = int64_t;     // UTC seconds

// Sentinel for block_index when a branch is empty (used in BranchTipInfo)
inline constexpr BlockIndex EMPTY_BRANCH_INDEX = static_cast<BlockIndex>(-1);

// ── Enumerations ──────────────────────────────────────────────────────────────

enum class BlockType : uint8_t {
    DATA,
    MERGE,
    KEY_ROTATION,
    REVOCATION,
};

enum class SealMode : uint8_t {
    BLIND,
    OPEN,
};

// ── Free tree utilities ───────────────────────────────────────────────────────

// ⌊log₂(index + 1)⌋  (root depth = 0, leaves at depth 31)
inline uint32_t node_depth(NodeIndex index) noexcept {
    if (index == 0) return 0;
    // using 64-bit to avoid overflow when index == UINT32_MAX
    return static_cast<uint32_t>(63 - __builtin_clzll(static_cast<uint64_t>(index) + 1));
}

inline bool is_leaf_node(NodeIndex index) noexcept {
    return node_depth(index) == 31;
}

// Any existing heap position (depth 0–31). Branches may grow from any valid
// node (blockchain.md §3.2 v0.7); only depth-31 nodes have no child nodes.
inline bool is_valid_node(NodeIndex index) noexcept {
    return node_depth(index) <= 31;
}

// Strict-ancestor check via heap arithmetic (parent = (n-1)/2).
// A node is not its own ancestor.
inline bool is_ancestor(NodeIndex ancestor, NodeIndex descendant) noexcept {
    if (descendant <= ancestor) return false;
    NodeIndex cur = descendant;
    while (cur > ancestor) cur = (cur - 1) / 2;
    return cur == ancestor;
}

// Returns heap indices [0, ..., target] from root to target (inclusive).
inline std::vector<NodeIndex> path_indices(NodeIndex target) noexcept {
    std::vector<NodeIndex> path;
    NodeIndex cur = target;
    while (cur != 0) {
        path.push_back(cur);
        cur = (cur - 1) / 2;
    }
    path.push_back(0);
    std::reverse(path.begin(), path.end());
    return path;
}

// ── Block address ─────────────────────────────────────────────────────────────

struct BlockAddress {
    UserId     user_id;
    NodeIndex  node_index;
    BlockIndex block_index;

    bool operator==(const BlockAddress& o) const noexcept {
        return user_id == o.user_id
            && node_index  == o.node_index
            && block_index == o.block_index;
    }
};

struct ExternalRef {
    BlockAddress address;
    Hash         block_hash;

    bool operator==(const ExternalRef& o) const noexcept {
        return address == o.address && block_hash == o.block_hash;
    }
};

// ── Node ──────────────────────────────────────────────────────────────────────

struct Node {
    NodeIndex  index;
    PublicKey  structural_pubkey;   // in MVP same as working_pubkey
    PublicKey  working_pubkey;
    Hash       parent_hash;         // Hash::zero() for root
    Signature  parent_sig;          // self-signed for root
    Timestamp  created_at;

    uint32_t  depth()             const noexcept { return node_depth(index); }
    bool      is_root()           const noexcept { return index == 0; }
    bool      is_leaf()           const noexcept { return node_depth(index) == 31; }
    bool      is_left_child()     const noexcept { return index > 0 && (index % 2 == 1); }
    NodeIndex parent_index()      const noexcept { return (index - 1) / 2; }
    NodeIndex left_child_index()  const noexcept { return 2 * index + 1; }
    NodeIndex right_child_index() const noexcept { return 2 * index + 2; }

    bool operator==(const Node& o) const noexcept {
        return index              == o.index
            && structural_pubkey == o.structural_pubkey
            && working_pubkey    == o.working_pubkey
            && parent_hash       == o.parent_hash
            && parent_sig        == o.parent_sig
            && created_at        == o.created_at;
    }
};

// ── Block ─────────────────────────────────────────────────────────────────────

struct Block {
    BlockAddress             address;
    Hash                     prev_hash;
    Timestamp                timestamp_claimed;
    BlockType                type;
    std::vector<uint8_t>     payload;
    std::vector<ExternalRef> external_refs;
    Signature                signature;
    // co_signature is excluded from canonical hash (see blockchain-api.md §5.4)
    std::optional<Signature> co_signature;
};

// ── Payload structures ────────────────────────────────────────────────────────

struct MergePayload {
    BlockAddress partner_last_address;
    Hash         partner_last_hash;
    Timestamp    merge_timestamp;
    // Snapshot commitments (blockchain.md §6.5.1). All part of the signed block.
    Hash         merkle_root;      // hierarchical Merkle root over the participant set
    Hash         hll_hash;         // BLAKE2b of the HLL register array (unique-count commitment)
    uint32_t     validated_depth;  // self-declared full-validation depth (§6.5.5)
};

struct KeyRotationPayload {
    PublicKey new_working_pubkey;
};

// Key revocation (§6.7). Lives in a branch of a strict ancestor of the revoked
// node; signed by that ancestor branch's working key (priority gradient §4.4).
struct RevocationPayload {
    NodeIndex                revoked_node_index;
    PublicKey                revoked_pubkey;     // the exact key being revoked
    Timestamp                compromised_since;
    // absent = emergency stop; a replacement may be assigned later by a second
    // REVOCATION block (§6.7 rule 3)
    std::optional<PublicKey> replacement_pubkey;
};

// ── Seal ──────────────────────────────────────────────────────────────────────

struct Seal {
    UserId    signer_id;
    Hash      block_hash;
    Signature signature;
    SealMode  mode;
    Timestamp sealed_at;

    bool operator==(const Seal& o) const noexcept {
        return signer_id == o.signer_id
            && block_hash == o.block_hash
            && signature  == o.signature
            && mode       == o.mode
            && sealed_at  == o.sealed_at;
    }
};

// ── Branch tip info (used in merge protocol §6.4) ─────────────────────────────

struct BranchTipInfo {
    BlockAddress         tip_address;  // block_index == EMPTY_BRANCH_INDEX if branch is empty
    Hash                 tip_hash;     // hash of last block, or hash of leaf node if empty
    std::vector<Node>    path;         // nodes from root to leaf (inclusive)
    std::optional<Block> tip_block;    // last block; nullopt when branch is empty
};

// ── Pending merge block (intermediate state in MergeSession) ──────────────────

struct PendingMergeBlock {
    Block draft;       // signed by own key; co_signature == nullopt
    Hash  draft_hash;  // Crypto::hash_block(draft) — sent to partner for co-signing
};

} // namespace blockchain
