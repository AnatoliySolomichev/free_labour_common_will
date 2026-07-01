#include "blockchain/hll.h"
#include <gtest/gtest.h>
#include <cmath>
#include <cstdint>

using namespace blockchain;

// ── Helpers ───────────────────────────────────────────────────────────────────

static UserId make_user(uint32_t i) {
    UserId u{};
    u.bytes[0] = static_cast<uint8_t>(i);
    u.bytes[1] = static_cast<uint8_t>(i >> 8);
    u.bytes[2] = static_cast<uint8_t>(i >> 16);
    u.bytes[3] = static_cast<uint8_t>(i >> 24);
    return u;
}

static double rel_err(uint64_t est, uint64_t truth) {
    return std::abs(static_cast<double>(est) - static_cast<double>(truth))
         / static_cast<double>(truth);
}

// ── basic cardinality ─────────────────────────────────────────────────────────

TEST(Hll, EmptyEstimateIsZero) {
    HllSketch s;
    EXPECT_EQ(s.estimate(), 0u);
}

TEST(Hll, SingleAddIsOne) {
    HllSketch s;
    s.add(make_user(42));
    EXPECT_EQ(s.estimate(), 1u);
}

TEST(Hll, DuplicateIdCountsOnce) {
    HllSketch s;
    for (int i = 0; i < 5; ++i)
        s.add(make_user(7));   // same identity five times
    EXPECT_EQ(s.estimate(), 1u);
}

TEST(Hll, EstimateAccuracyLarge) {
    HllSketch s;
    constexpr uint32_t N = 2000;
    for (uint32_t i = 0; i < N; ++i)
        s.add(make_user(i));
    EXPECT_LT(rel_err(s.estimate(), N), 0.10) << "est=" << s.estimate();
}

// ── merge = set union with dedup ──────────────────────────────────────────────

TEST(Hll, MergeEstimatesUnion) {
    HllSketch a, b;
    for (uint32_t i = 0;   i < 500; ++i) a.add(make_user(i));  // {0..499}
    for (uint32_t i = 250; i < 750; ++i) b.add(make_user(i));  // {250..749}, overlap 250..499

    a.merge(b);   // union = {0..749} = 750 unique
    EXPECT_LT(rel_err(a.estimate(), 750), 0.10) << "est=" << a.estimate();
}

TEST(Hll, MergeEqualsDirectUnion) {
    HllSketch a, b, combined;
    for (uint32_t i = 0;   i < 500; ++i) a.add(make_user(i));
    for (uint32_t i = 250; i < 750; ++i) b.add(make_user(i));
    for (uint32_t i = 0;   i < 750; ++i) combined.add(make_user(i));

    a.merge(b);
    EXPECT_EQ(a, combined);                          // element-wise max == union registers
    EXPECT_EQ(a.sketch_hash(), combined.sketch_hash());
}

TEST(Hll, MergeIsCommutative) {
    HllSketch a1, b1, a2, b2;
    for (uint32_t i = 0;   i < 300; ++i) { a1.add(make_user(i)); a2.add(make_user(i)); }
    for (uint32_t i = 200; i < 600; ++i) { b1.add(make_user(i)); b2.add(make_user(i)); }

    a1.merge(b1);   // a ∪ b
    b2.merge(a2);   // b ∪ a
    EXPECT_EQ(a1, b2);
}

// ── commitment hash ───────────────────────────────────────────────────────────

TEST(Hll, SketchHashDeterministic) {
    HllSketch a, b;
    for (uint32_t i = 0; i < 100; ++i) { a.add(make_user(i)); b.add(make_user(i)); }
    EXPECT_EQ(a.sketch_hash(), b.sketch_hash());
}

TEST(Hll, SketchHashChangesWithContent) {
    HllSketch empty;
    Hash h0 = empty.sketch_hash();

    HllSketch filled;
    for (uint32_t i = 0; i < 1000; ++i) filled.add(make_user(i));
    EXPECT_NE(h0, filled.sketch_hash());
}

// ── transport round-trip ──────────────────────────────────────────────────────

TEST(Hll, FromRegistersRoundTrip) {
    HllSketch s;
    for (uint32_t i = 0; i < 400; ++i) s.add(make_user(i));

    HllSketch copy = HllSketch::from_registers(s.registers());
    EXPECT_EQ(copy, s);
    EXPECT_EQ(copy.estimate(), s.estimate());
    EXPECT_EQ(copy.sketch_hash(), s.sketch_hash());
}
