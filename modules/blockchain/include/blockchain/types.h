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

// How far two honest clocks may disagree (blockchain.md §9.4): a time later than
// someone's now by more than this comes from the future.
inline constexpr Timestamp kClockSlack = 600;

// Sentinel for block_index when a branch is empty (used in BranchTipInfo, and
// in a revocation line that holds no block: blockchain.md §6.7 rule 12).
inline constexpr BlockIndex EMPTY_BRANCH_INDEX = static_cast<BlockIndex>(-1);

// ── Enumerations ──────────────────────────────────────────────────────────────

enum class BlockType : uint8_t {
    DATA,
    MERGE,
    KEY_ROTATION,
    REVOCATION,
    BIRTH,        // a node is born: in its parent's branch (blockchain.md §11.2)
    KEY_REPLACEMENT,  // the branch takes the key an ancestor's revocation assigned,
                      // right after the owner's line (blockchain.md §6.7 rule 9)
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

// Where a node was born (blockchain.md §11.2): its BIRTH block in the parent's
// branch. Zero for the root, which has no parent.
struct BirthRef {
    BlockIndex index = 0;
    Hash       hash{};
    bool operator==(const BirthRef& o) const noexcept {
        return index == o.index && hash == o.hash;
    }
};

// A node of the tree as a storage knows it (blockchain.md §3.3, blockchain.md §11.2): for the
// root, its own self-signed record; for any other node, what its BIRTH block in
// the parent's branch says — index, keys, and where that block stands. A node
// is signed once: the root by itself, any other by its parent, in the birth
// block (решение участника 2026-11-02).
struct Node {
    NodeIndex  index;
    PublicKey  structural_pubkey;   // in MVP same as working_pubkey
    PublicKey  working_pubkey;
    BirthRef   birth;               // the BIRTH block in the parent's branch; zero for the root
    Signature  root_sig;            // the root's self-signature; null for any other node

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
            && birth             == o.birth
            && root_sig          == o.root_sig;
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
};

// ── Payload structures ────────────────────────────────────────────────────────

struct MergePayload {
    BlockAddress partner_last_address;
    Hash         partner_last_hash;
    Timestamp    merge_timestamp;
    // Snapshot commitments (blockchain.md §6.5.1). All part of the signed block.
    Hash         merkle_root;      // hierarchical Merkle root over the participant set
    Hash         hll_hash;         // BLAKE2b of the HLL register array (unique-count commitment)
    uint32_t     validated_depth;  // self-declared full-validation depth (blockchain.md §6.5.5)
};

// A node's birth (blockchain.md §11.2): who is born and with which keys.
struct BirthPayload {
    NodeIndex child_index;
    PublicKey structural_pubkey;
    PublicKey working_pubkey;
};

struct KeyRotationPayload {
    PublicKey new_working_pubkey;
};

// The branch takes a replacement key (blockchain.md §6.7 rule 9): the
// KEY_REPLACEMENT block names the REVOCATION record that assigned the key; its
// prev_hash names the owner's last valid block it stands right after.
struct ReplacementPayload {
    BlockAddress record_address;
    Hash         record_hash;
};

// Key revocation (blockchain.md §6.7). Lives in a branch of a strict ancestor of the revoked
// node; signed by that ancestor branch's working key (priority gradient blockchain.md §4.4).
// The owner's last valid block of a revoked branch (blockchain.md §6.7 rule 12): the
// index tells any verifier how long the owner's line is, the hash — which
// block ends it.
struct LastGood {
    BlockIndex index;
    Hash       hash;

    bool operator==(const LastGood& o) const noexcept {
        return index == o.index && hash == o.hash;
    }
};

struct RevocationPayload {
    NodeIndex                revoked_node_index;
    PublicKey                revoked_pubkey;     // the exact key being revoked
    // absent = emergency stop; a replacement may be assigned later by a second
    // REVOCATION block (blockchain.md §6.7 rule 3)
    std::optional<PublicKey> replacement_pubkey;
    // The owner's last valid block of the revoked branch: the prev_hash line
    // ending here is the owner's, the rest of the revoked key is not
    // (blockchain.md §6.7 rule 12). Absent — zones by time (rule 5).
    std::optional<LastGood>  last_good;
};

// ── Key path (blockchain.md §7.5) ─────────────────────────────────────────────

// Where a branch's keys come from, self-verifying (KeyPathCert): the root's
// record, the BIRTH block of every node below it down to the branch's node, and
// every key change of the path's nodes — KEY_ROTATION blocks, signed by the key
// before them, and KEY_REPLACEMENT blocks, signed by the replacement, with the
// REVOCATION records that assigned those replacements (blockchain.md §6.6,
// blockchain.md §6.7 rule 9). A node's signature at place i of its branch is
// checked by the key in force there. Nodes are not carried: each is what its
// birth says (решение участника 2026-11-02).
struct KeyPath {
    Node               root;      // the root's self-signed record
    std::vector<Block> births;    // root's child first, down to the branch's node
    std::vector<Block> changes;   // key changes of the path's nodes: node by node, root first, in branch order
    std::vector<Block> records;   // the REVOCATION records the KEY_REPLACEMENT blocks name
};

// ── Branch tip info (used in merge protocol blockchain.md §6.4) ─────────────────────────────

struct BranchTipInfo {
    BlockAddress         tip_address;  // block_index == EMPTY_BRANCH_INDEX if branch is empty
    Hash                 tip_hash;     // hash of the last block, or of the branch's origin if empty
    KeyPath              path;         // root → the branch's node, with every key change
    std::optional<Block> tip_block;    // last block; nullopt when branch is empty
};

} // namespace blockchain
