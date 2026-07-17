// Credit history off the emission thread — layer 1 of ИР-010
// (records.md §11.6). The thread declares debt as non-positive debt_after.

#include "records/credit.h"

#include <gtest/gtest.h>

using namespace records;

namespace {

std::array<uint8_t, 32> hash_of(uint8_t b) {
    std::array<uint8_t, 32> h{};
    h.fill(b);
    return h;
}

ObservedLink issue(uint64_t seq, double units, double debt_after,
                   int64_t ts = 0, uint8_t block = 0) {
    ObservedLink o{};
    o.link.seq        = seq;
    o.link.debt_after = debt_after;
    o.units           = units;
    o.is_redemption   = false;
    o.timestamp       = ts;
    o.block_hash      = hash_of(block ? block : static_cast<uint8_t>(seq + 1));
    return o;
}

ObservedLink redeem(uint64_t seq, double units, double debt_after,
                    int64_t ts = 0, uint8_t block = 0) {
    auto o = issue(seq, units, debt_after, ts, block);
    o.is_redemption = true;
    return o;
}

} // namespace

TEST(CreditHistory, EmptyThread) {
    const auto h = credit_history({});
    EXPECT_EQ(h.links_seen, 0u);
    EXPECT_EQ(h.links_expected, 0u);
    EXPECT_FALSE(h.gaps);
    EXPECT_DOUBLE_EQ(h.issued, 0);
    EXPECT_DOUBLE_EQ(h.outstanding, 0);
}

TEST(CreditHistory, SingleIssue) {
    const auto h = credit_history({issue(0, 10, -10, 100)});
    EXPECT_DOUBLE_EQ(h.issued, 10);
    EXPECT_DOUBLE_EQ(h.redeemed, 0);
    EXPECT_DOUBLE_EQ(h.outstanding, 10);
    EXPECT_DOUBLE_EQ(h.max_debt, 10);
    EXPECT_DOUBLE_EQ(h.max_repaid, 0);
    EXPECT_FALSE(h.gaps);
    // One issue is a young thread, not yet "growth without redemption".
    EXPECT_FALSE(h.growth_without_redemption);
    EXPECT_FALSE(h.thread_inconsistent);
    ASSERT_TRUE(h.last_selfissue_ts.has_value());
    EXPECT_EQ(*h.last_selfissue_ts, 100);
    EXPECT_FALSE(h.last_redemption_ts.has_value());
}

TEST(CreditHistory, FullCycleRepaysPeak) {
    // Borrow 10, repay 4, borrow 5 (debt 11), repay all 11.
    const auto h = credit_history({
        issue (0, 10, -10, 10),
        redeem(1, 4,  -6,  20),
        issue (2, 5,  -11, 30),
        redeem(3, 11, 0,   40),
    });
    EXPECT_DOUBLE_EQ(h.issued, 15);
    EXPECT_DOUBLE_EQ(h.redeemed, 15);
    EXPECT_DOUBLE_EQ(h.outstanding, 0);
    EXPECT_DOUBLE_EQ(h.max_debt, 11);
    EXPECT_DOUBLE_EQ(h.max_repaid, 11);
    EXPECT_FALSE(h.gaps);
    EXPECT_FALSE(h.growth_without_redemption);
    EXPECT_FALSE(h.thread_inconsistent);
    EXPECT_EQ(*h.last_redemption_ts, 40);
}

TEST(CreditHistory, PartialRepayDrawdown) {
    // Peak 12, repaid down to 7: max_repaid is 5, not 12.
    const auto h = credit_history({
        issue (0, 12, -12),
        redeem(1, 5,  -7),
    });
    EXPECT_DOUBLE_EQ(h.max_debt, 12);
    EXPECT_DOUBLE_EQ(h.max_repaid, 5);
    EXPECT_DOUBLE_EQ(h.outstanding, 7);
}

TEST(CreditHistory, GapsMakePartialViewExplicit) {
    // Links 0 and 3 seen, 1–2 missing: an honest partial replica.
    const auto h = credit_history({
        issue(0, 10, -10),
        issue(3, 2,  -7),   // debt shrank meanwhile — redemptions not seen
    });
    EXPECT_EQ(h.links_seen, 2u);
    EXPECT_EQ(h.links_expected, 4u);
    EXPECT_TRUE(h.gaps);
    // No slander on partial data: flags must stay down.
    EXPECT_FALSE(h.growth_without_redemption);
    EXPECT_FALSE(h.thread_inconsistent);
}

TEST(CreditHistory, GrowthWithoutRedemptionFlag) {
    const auto h = credit_history({
        issue(0, 5, -5),
        issue(1, 5, -10),
        issue(2, 5, -15),
    });
    EXPECT_FALSE(h.gaps);
    EXPECT_TRUE(h.growth_without_redemption);
}

TEST(CreditHistory, InconsistentThreadFlag) {
    // Declared debt (2) disagrees with issued − redeemed (10) on a full view.
    const auto h = credit_history({
        issue(0, 10, -10),
        issue(1, 0,  -2),   // declares debt shrank without a redemption
    });
    EXPECT_FALSE(h.gaps);
    EXPECT_TRUE(h.thread_inconsistent);
}

TEST(CreditHistory, SameBlockTwiceIsNotEquivocation) {
    const auto h = credit_history({
        issue(0, 10, -10, 10, /*block=*/7),
        issue(0, 10, -10, 10, /*block=*/7),
    });
    EXPECT_TRUE(h.equivocated_seqs.empty());
    EXPECT_EQ(h.links_seen, 1u);
    EXPECT_DOUBLE_EQ(h.issued, 10);
}

TEST(CreditHistory, ForkedLinkIsEquivocation) {
    // One seq, two different carrier blocks — the thread forked.
    const auto h = credit_history({
        issue(0, 10, -10, 10, /*block=*/1),
        issue(1, 3,  -13, 20, /*block=*/2),
        issue(1, 8,  -18, 21, /*block=*/3),
    });
    ASSERT_EQ(h.equivocated_seqs.size(), 1u);
    EXPECT_EQ(h.equivocated_seqs[0], 1u);
    EXPECT_EQ(h.links_seen, 2u);  // forked seq counted once
}

TEST(CreditHistory, UnorderedInputIsSorted) {
    const auto h = credit_history({
        redeem(1, 4, -6, 20),
        issue (0, 10, -10, 10),
    });
    EXPECT_DOUBLE_EQ(h.outstanding, 6);
    EXPECT_DOUBLE_EQ(h.max_repaid, 4);
    EXPECT_FALSE(h.gaps);
}
