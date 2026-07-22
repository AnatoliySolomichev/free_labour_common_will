// Carry thread of a means of production (ИР-011, records.md §9.4 v2):
// carried = min(used/capacity × cost, cost − collected_before), one linear
// thread per asset record, forced zero after full recovery, fork = double
// charge. The reference asset in most tests: cost 100 h, life 1000 h.

#include "records/carry.h"

#include <gtest/gtest.h>

using namespace records;

namespace {

constexpr double COST = 100.0;
constexpr double LIFE = 1000.0;

std::array<uint8_t, 32> hash_of(uint8_t b) {
    std::array<uint8_t, 32> h{};
    h.fill(b);
    return h;
}

ObservedCarry link(uint64_t seq, double used, double carried, double after,
                   uint8_t block = 0) {
    ObservedCarry o{};
    o.entry.seq     = seq;
    o.entry.used    = used;
    o.entry.carried = carried;
    o.entry.after   = after;
    o.block_hash    = hash_of(block ? block : static_cast<uint8_t>(seq + 1));
    return o;
}

} // namespace

// ── carry_step: the recognised transfer for one use ──────────────────────────

TEST(CarryStep, LinearShareOfCost) {
    // 10 tool-hours of a 1000-hour life carry 1% of the cost.
    EXPECT_DOUBLE_EQ(carry_step(COST, LIFE, 10, 0), 1.0);
}

TEST(CarryStep, CappedByRemainder) {
    // 50 hours would carry 5 h, but only 2 h of cost remain uncollected.
    EXPECT_DOUBLE_EQ(carry_step(COST, LIFE, 50, 98), 2.0);
}

TEST(CarryStep, ForcedZeroAfterFullRecovery) {
    // Fully amortised asset keeps working for free — not an option, the rule.
    EXPECT_DOUBLE_EQ(carry_step(COST, LIFE, 10, COST), 0.0);
    EXPECT_DOUBLE_EQ(carry_step(COST, LIFE, 10, COST + 5), 0.0);
}

TEST(CarryStep, DegenerateInputs) {
    EXPECT_DOUBLE_EQ(carry_step(0, LIFE, 10, 0), 0.0);
    EXPECT_DOUBLE_EQ(carry_step(COST, 0, 10, 0), 0.0);
    EXPECT_DOUBLE_EQ(carry_step(COST, LIFE, 0, 0), 0.0);
}

// ── carry_history: digesting an observed thread ──────────────────────────────

TEST(CarryHistory, EmptyThread) {
    const auto h = carry_history({}, COST, LIFE);
    EXPECT_EQ(h.links_seen, 0u);
    EXPECT_FALSE(h.gaps);
    EXPECT_DOUBLE_EQ(h.collected, 0);
}

TEST(CarryHistory, HonestThread) {
    const auto h = carry_history({link(0, 10, 1.0, 1.0),
                                  link(1, 20, 2.0, 3.0),
                                  link(2, 5, 0.5, 3.5)},
                                 COST, LIFE);
    EXPECT_EQ(h.links_seen, 3u);
    EXPECT_EQ(h.links_expected, 3u);
    EXPECT_FALSE(h.gaps);
    EXPECT_DOUBLE_EQ(h.collected, 3.5);
    EXPECT_FALSE(h.formula_mismatch);
    EXPECT_FALSE(h.over_invariant);
    EXPECT_FALSE(h.after_decreasing);
    EXPECT_FALSE(h.thread_inconsistent);
    EXPECT_TRUE(h.equivocated_seqs.empty());
}

TEST(CarryHistory, OrderIndependentAndDuplicateBlocksTolerated) {
    // The same block via merge and via fetch is not equivocation.
    const auto a = link(0, 10, 1.0, 1.0);
    const auto h = carry_history({link(1, 20, 2.0, 3.0), a, a}, COST, LIFE);
    EXPECT_EQ(h.links_seen, 2u);
    EXPECT_TRUE(h.equivocated_seqs.empty());
    EXPECT_FALSE(h.gaps);
}

TEST(CarryHistory, GapReportedNotSlandered) {
    // Links 0 and 2 seen, 1 missing: partial view, no objective flags.
    const auto h = carry_history({link(0, 10, 1.0, 1.0),
                                  link(2, 10, 1.0, 3.0)},
                                 COST, LIFE);
    EXPECT_TRUE(h.gaps);
    EXPECT_EQ(h.links_seen, 2u);
    EXPECT_EQ(h.links_expected, 3u);
    // The jump 1.0 → 3.0 is explained by the unseen link 1.
    EXPECT_FALSE(h.thread_inconsistent);
    EXPECT_FALSE(h.formula_mismatch);
}

TEST(CarryHistory, FormulaMismatchCaught) {
    // Claims 5 h carried for 10 tool-hours — five times the linear share.
    const auto h = carry_history({link(0, 10, 5.0, 5.0)}, COST, LIFE);
    EXPECT_TRUE(h.formula_mismatch);
}

TEST(CarryHistory, OverInvariantIsMoneyPump) {
    // after > cost: the asset has transferred more than was ever invested.
    const auto h = carry_history({link(0, 10, 1.0, 101.0)}, COST, LIFE);
    EXPECT_TRUE(h.over_invariant);
}

TEST(CarryHistory, ForcedZeroingAcceptedAtTheTail) {
    // Asset nearly recovered: link 1 carries only the 0.5 h remainder,
    // link 2 works for free. Both must validate cleanly.
    const auto h = carry_history({link(0, 995, 99.5, 99.5),
                                  link(1, 50, 0.5, 100.0),
                                  link(2, 40, 0.0, 100.0)},
                                 COST, LIFE);
    EXPECT_FALSE(h.formula_mismatch);
    EXPECT_FALSE(h.over_invariant);
    EXPECT_DOUBLE_EQ(h.collected, COST);
}

TEST(CarryHistory, AdjacentBreakIsObjective) {
    // seq 0 and 1 both seen: after must be continuous. 1.0 + 2.0 ≠ 4.0.
    const auto h = carry_history({link(0, 10, 1.0, 1.0),
                                  link(1, 20, 2.0, 4.0)},
                                 COST, LIFE);
    EXPECT_TRUE(h.thread_inconsistent);
}

TEST(CarryHistory, AfterDecreasingImpossible) {
    const auto h = carry_history({link(0, 30, 3.0, 3.0),
                                  link(2, 10, 1.0, 2.0)},
                                 COST, LIFE);
    EXPECT_TRUE(h.after_decreasing);
}

TEST(CarryHistory, EquivocationIsDoubleCharge) {
    // One seq in two different blocks: the drill was amortised in two
    // branches in parallel — the fork the thread exists to catch.
    const auto h = carry_history({link(0, 10, 1.0, 1.0),
                                  link(1, 20, 2.0, 3.0, 0x51),
                                  link(1, 20, 2.0, 3.0, 0x52)},
                                 COST, LIFE);
    ASSERT_EQ(h.equivocated_seqs.size(), 1u);
    EXPECT_EQ(h.equivocated_seqs[0], 1u);
}

// ── Reissue arithmetic (records.md §10.2): the global invariant ───────────────

TEST(CarryReissue, GlobalInvariantAcrossOwners) {
    // Owner A: cost 100, collects 60. Resale ceiling = 40.
    const auto a = carry_history({link(0, 600, 60.0, 60.0)}, COST, LIFE);
    const double ceiling = COST - a.collected;
    EXPECT_DOUBLE_EQ(ceiling, 40.0);

    // Owner B buys at the ceiling; may collect at most 40 in total:
    // Σ over the instance's life = 60 + 40 ≤ 100 — no money pump.
    const auto b = carry_history({link(0, 400, 40.0, 40.0)}, ceiling, 400.0);
    EXPECT_FALSE(b.over_invariant);
    EXPECT_DOUBLE_EQ(a.collected + b.collected, COST);
}
