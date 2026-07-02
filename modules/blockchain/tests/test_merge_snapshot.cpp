#include "blockchain/merge_snapshot.h"
#include "blockchain/merkle.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>

using namespace blockchain;

// ── Helpers ───────────────────────────────────────────────────────────────────

static ExternalRef make_ref(uint32_t user, NodeIndex node, BlockIndex block) {
    ExternalRef r{};
    r.address.user_id.bytes[0] = static_cast<uint8_t>(user);
    r.address.user_id.bytes[1] = static_cast<uint8_t>(user >> 8);
    r.address.user_id.bytes[2] = static_cast<uint8_t>(user >> 16);
    r.address.user_id.bytes[3] = static_cast<uint8_t>(user >> 24);
    r.address.node_index  = node;
    r.address.block_index = block;
    r.block_hash.bytes.fill(static_cast<uint8_t>(user));
    return r;
}

// Fold participants [lo, hi) into one snapshot by repeated left-leaning merge.
static MergeSnapshot fold(uint32_t lo, uint32_t hi) {
    MergeSnapshot s = MergeSnapshot::leaf(make_ref(lo, lo, 0));
    for (uint32_t i = lo + 1; i < hi; ++i)
        s = MergeSnapshot::merge(s, MergeSnapshot::leaf(make_ref(i, i, 0)));
    return s;
}

static double rel_err(uint64_t est, uint64_t truth) {
    return std::abs(static_cast<double>(est) - static_cast<double>(truth))
         / static_cast<double>(truth);
}

// ── leaf ──────────────────────────────────────────────────────────────────────

TEST(MergeSnapshot, LeafRootIsLeafHash) {
    ExternalRef r = make_ref(1, 10, 5);
    MergeSnapshot s = MergeSnapshot::leaf(r);
    EXPECT_EQ(s.merkle_root, MerkleTree::leaf_hash(r));
    EXPECT_EQ(s.estimate(), 1u);
}

// ── merge: hierarchical root ──────────────────────────────────────────────────

TEST(MergeSnapshot, MergeTwoLeavesRootIsCanonicalCombine) {
    ExternalRef a = make_ref(1, 1, 0), b = make_ref(2, 2, 0);
    Hash la = MerkleTree::leaf_hash(a);
    Hash lb = MerkleTree::leaf_hash(b);
    Hash expected = (la.bytes <= lb.bytes) ? MerkleTree::combine(la, lb)
                                           : MerkleTree::combine(lb, la);

    MergeSnapshot s = MergeSnapshot::merge(MergeSnapshot::leaf(a),
                                           MergeSnapshot::leaf(b));
    EXPECT_EQ(s.merkle_root, expected);
    EXPECT_EQ(s.estimate(), 2u);
}

TEST(MergeSnapshot, MergeIsCommutative) {
    MergeSnapshot a = MergeSnapshot::leaf(make_ref(1, 1, 0));
    MergeSnapshot b = MergeSnapshot::leaf(make_ref(2, 2, 0));

    MergeSnapshot ab = MergeSnapshot::merge(a, b);
    MergeSnapshot ba = MergeSnapshot::merge(b, a);
    EXPECT_EQ(ab, ba);
    EXPECT_EQ(ab.sketch_hash(), ba.sketch_hash());
}

// ── merge: unique-count via HLL ───────────────────────────────────────────────

TEST(MergeSnapshot, MergeCountsUniqueParticipants) {
    MergeSnapshot a = fold(0, 100);     // {0..99}
    MergeSnapshot b = fold(50, 150);    // {50..149}, overlap 50..99

    MergeSnapshot u = MergeSnapshot::merge(a, b);  // union {0..149} = 150 unique
    EXPECT_LT(rel_err(u.estimate(), 150), 0.10) << "est=" << u.estimate();
}

TEST(MergeSnapshot, SketchHashMatchesInternalHll) {
    MergeSnapshot s = fold(0, 40);
    EXPECT_EQ(s.sketch_hash(), s.hll.sketch_hash());
}

// ── determinism ───────────────────────────────────────────────────────────────

TEST(MergeSnapshot, DeterministicFold) {
    EXPECT_EQ(fold(0, 64), fold(0, 64));
}
