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

// ⌊log₂(index + 1)⌋  (root depth = 0, leaves at depth 32)
inline uint32_t node_depth(NodeIndex index) noexcept {
    if (index == 0) return 0;
    // using 64-bit to avoid overflow when index == UINT32_MAX
    return static_cast<uint32_t>(63 - __builtin_clzll(static_cast<uint64_t>(index) + 1));
}

inline bool is_leaf_node(NodeIndex index) noexcept {
    return node_depth(index) == 32;
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
    bool      is_leaf()           const noexcept { return node_depth(index) == 32; }
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
};

struct KeyRotationPayload {
    PublicKey new_working_pubkey;
};

struct RevocationPayload {
    NodeIndex revoked_node_index;
    Timestamp compromised_since;
    PublicKey replacement_pubkey;
    // [OPEN §11.1] placement of this block in the tree is an open question
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
    BlockAddress      tip_address;  // block_index == EMPTY_BRANCH_INDEX if branch is empty
    Hash              tip_hash;     // hash of last block, or hash of leaf node if empty
    std::vector<Node> path;         // nodes from root to leaf (inclusive)
};

// ── Pending merge block (intermediate state in MergeSession) ──────────────────

struct PendingMergeBlock {
    Block draft;       // signed by own key; co_signature == nullopt
    Hash  draft_hash;  // Crypto::hash_block(draft) — sent to partner for co-signing
};

} // namespace blockchain
