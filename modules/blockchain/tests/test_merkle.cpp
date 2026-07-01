#include "blockchain/merkle.h"
#include "blockchain/crypto.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>

using namespace blockchain;

// ── Helpers ───────────────────────────────────────────────────────────────────

static Hash make_hash(uint8_t fill) {
    Hash h;
    h.bytes.fill(fill);
    return h;
}

static std::vector<Hash> make_leaves(size_t n) {
    std::vector<Hash> v;
    v.reserve(n);
    for (size_t i = 0; i < n; ++i)
        v.push_back(make_hash(static_cast<uint8_t>(0x10 + i)));
    return v;
}

static ExternalRef make_ref(uint8_t fill, NodeIndex node, BlockIndex block) {
    ExternalRef r{};
    r.address.user_id.bytes.fill(fill);
    r.address.node_index  = node;
    r.address.block_index = block;
    r.block_hash.bytes.fill(static_cast<uint8_t>(fill + 1));
    return r;
}

// ── root ──────────────────────────────────────────────────────────────────────

TEST(Merkle, RootEmptyIsZero) {
    EXPECT_EQ(MerkleTree::root({}), Hash::zero());
}

TEST(Merkle, RootSingleLeafIsLeaf) {
    Hash leaf = make_hash(0xAA);
    EXPECT_EQ(MerkleTree::root({leaf}), leaf);
}

TEST(Merkle, RootIsDeterministic) {
    auto leaves = make_leaves(5);
    EXPECT_EQ(MerkleTree::root(leaves), MerkleTree::root(leaves));
}

TEST(Merkle, InternalNodeDiffersFromLeaves) {
    // Root of two leaves is an internal node: must not collide with either leaf.
    auto leaves = make_leaves(2);
    Hash root = MerkleTree::root(leaves);
    EXPECT_NE(root, leaves[0]);
    EXPECT_NE(root, leaves[1]);
}

TEST(Merkle, DifferentLeafOrderDifferentRoot) {
    Hash a = make_hash(0x01), b = make_hash(0x02);
    EXPECT_NE(MerkleTree::root({a, b}), MerkleTree::root({b, a}));
}

// ── proofs (single leaf) ──────────────────────────────────────────────────────

TEST(Merkle, SingleLeafProofIsEmpty) {
    Hash leaf = make_hash(0xAA);
    auto proof = MerkleTree::make_proof({leaf}, 0);
    EXPECT_TRUE(proof.path.empty());
    EXPECT_TRUE(MerkleTree::verify(leaf, proof, leaf));
}

// ── proofs across all sizes and indices ───────────────────────────────────────

TEST(Merkle, AllIndicesVerifyForSizes1To9) {
    for (size_t n = 1; n <= 9; ++n) {
        auto leaves = make_leaves(n);
        Hash root   = MerkleTree::root(leaves);
        for (size_t i = 0; i < n; ++i) {
            auto proof = MerkleTree::make_proof(leaves, i);
            EXPECT_TRUE(MerkleTree::verify(leaves[i], proof, root))
                << "n=" << n << " i=" << i;
            EXPECT_EQ(MerkleTree::apply_proof(leaves[i], proof), root)
                << "n=" << n << " i=" << i;
        }
    }
}

TEST(Merkle, WrongLeafDoesNotVerify) {
    auto leaves = make_leaves(7);
    Hash root   = MerkleTree::root(leaves);
    auto proof  = MerkleTree::make_proof(leaves, 3);
    EXPECT_FALSE(MerkleTree::verify(make_hash(0xFF), proof, root));
}

// ── verification rejection cases ──────────────────────────────────────────────

TEST(Merkle, VerifyRejectsWrongRoot) {
    auto leaves = make_leaves(4);
    auto proof  = MerkleTree::make_proof(leaves, 1);
    EXPECT_FALSE(MerkleTree::verify(leaves[1], proof, make_hash(0x00)));
}

TEST(Merkle, VerifyRejectsTamperedSibling) {
    auto leaves = make_leaves(4);
    Hash root   = MerkleTree::root(leaves);
    auto proof  = MerkleTree::make_proof(leaves, 1);
    ASSERT_FALSE(proof.path.empty());
    proof.path[0].bytes[0] ^= 0xFF;
    EXPECT_FALSE(MerkleTree::verify(leaves[1], proof, root));
}

TEST(Merkle, VerifyRejectsFlippedDirection) {
    auto leaves = make_leaves(4);
    Hash root   = MerkleTree::root(leaves);
    auto proof  = MerkleTree::make_proof(leaves, 1);
    ASSERT_FALSE(proof.sibling_is_right.empty());
    proof.sibling_is_right[0] = !proof.sibling_is_right[0];
    EXPECT_FALSE(MerkleTree::verify(leaves[1], proof, root));
}

TEST(Merkle, VerifyRejectsMismatchedProofSizes) {
    auto leaves = make_leaves(4);
    Hash root   = MerkleTree::root(leaves);
    auto proof  = MerkleTree::make_proof(leaves, 1);
    proof.sibling_is_right.pop_back();   // sizes now differ
    EXPECT_FALSE(MerkleTree::verify(leaves[1], proof, root));
}

TEST(Merkle, MakeProofOutOfRangeThrows) {
    auto leaves = make_leaves(3);
    EXPECT_THROW(MerkleTree::make_proof(leaves, 3), InvalidArgumentError);
    EXPECT_THROW(MerkleTree::make_proof({}, 0),      InvalidArgumentError);
}

// ── leaf_hash over ExternalRef ────────────────────────────────────────────────

TEST(Merkle, LeafHashDeterministicAndDistinct) {
    ExternalRef a = make_ref(0x01, 10, 5);
    ExternalRef b = make_ref(0x01, 10, 5);
    ExternalRef c = make_ref(0x01, 10, 6);   // differs by block_index

    EXPECT_EQ(MerkleTree::leaf_hash(a), MerkleTree::leaf_hash(b));
    EXPECT_NE(MerkleTree::leaf_hash(a), MerkleTree::leaf_hash(c));
}

TEST(Merkle, SnapshotRoundTripFromRefs) {
    // Build a snapshot the way a MERGE block would: leaves = leaf_hash(ExternalRef).
    std::vector<Hash> leaves;
    for (uint8_t i = 0; i < 6; ++i)
        leaves.push_back(MerkleTree::leaf_hash(make_ref(i, i, i)));

    Hash root = MerkleTree::root(leaves);
    for (size_t i = 0; i < leaves.size(); ++i) {
        auto proof = MerkleTree::make_proof(leaves, i);
        EXPECT_TRUE(MerkleTree::verify(leaves[i], proof, root)) << "i=" << i;
    }
}
