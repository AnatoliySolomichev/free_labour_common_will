#pragma once

#include "types.h"
#include <cstddef>
#include <vector>

namespace blockchain {

// Binary Merkle tree over an ordered set of participant leaves.
//
// Commits to the participant snapshot of a MERGE block (blockchain.md §6.5.1)
// and provides the inclusion proofs verified by FraudClaim (records.md §3A.1).
//
// Second-preimage resistance via RFC 6962-style domain separation:
//   leaf_hash(ref)     = BLAKE2b(0x00 || CBOR(ExternalRef))
//   node_hash(l, r)    = BLAKE2b(0x01 || l || r)
// The distinct prefixes make it impossible to pass off an internal node as a
// leaf (or vice versa), so a proof cannot be reshaped into a different set.
//
// Odd count at a level: the last (unpaired) node is promoted unchanged to the
// next level. Combined with domain separation this stays unambiguous.
class MerkleTree {
public:
    MerkleTree() = delete;

    // Sibling hashes on the path from a leaf up to the root, bottom-up.
    // `sibling_is_right[i]` tells whether path[i] sits to the RIGHT of the
    // running hash at that level (i.e. combine as node_hash(running, sibling)).
    struct Proof {
        std::vector<Hash> path;
        std::vector<bool> sibling_is_right;

        bool operator==(const Proof& o) const noexcept {
            return path == o.path && sibling_is_right == o.sibling_is_right;
        }
    };

    // Leaf hash for a participant's ExternalRef (0x00-prefixed).
    // Throws: SerializationError.
    static Hash leaf_hash(const ExternalRef& ref);

    // Root over ordered leaf hashes (as produced by leaf_hash).
    // Empty set → Hash::zero(). Single leaf → that leaf unchanged.
    static Hash root(const std::vector<Hash>& leaves) noexcept;

    // Combine two child roots into a parent (hierarchical DAG composition, §6.5.1).
    // Equivalent to root({left, right}); exposed for MergeSnapshot.
    static Hash combine(const Hash& left, const Hash& right) noexcept;

    // Inclusion proof for the leaf at `leaf_index` in `leaves`.
    // Throws: InvalidArgumentError if leaf_index is out of range.
    static Proof make_proof(const std::vector<Hash>& leaves, size_t leaf_index);

    // Recompute the root from a leaf hash and its proof.
    static Hash apply_proof(const Hash& leaf, const Proof& proof) noexcept;

    // Check that leaf + proof reproduce expected_root.
    static bool verify(const Hash& leaf, const Proof& proof,
                       const Hash& expected_root) noexcept;
};

} // namespace blockchain
