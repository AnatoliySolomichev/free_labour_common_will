#include "records/acceptance.h"

#include <gtest/gtest.h>

using namespace records;

namespace {

using Hashes = std::set<std::array<uint8_t, 32>>;

std::array<uint8_t, 32> h(uint8_t fill) {
    std::array<uint8_t, 32> x{};
    x.fill(fill);
    return x;
}

DealClaim claim(uint8_t deal, uint8_t work) { return DealClaim{h(deal), h(work)}; }

constexpr uint8_t kWork = 0x10, kOther = 0x20;

} // namespace

// Одна сделка у работы — она и считается.
TEST(DealRecognition, LoneDealCounts) {
    const auto r = recognize_deals({claim(0xA1, kWork)});
    EXPECT_EQ(r.recognized, Hashes{h(0xA1)});
    EXPECT_TRUE(r.oversold.empty());
}

// Две сделки на одну работу — продана дважды: не считается ни одна, работа видна.
TEST(DealRecognition, TwoDealsOnOneWorkAreSoldTwice) {
    const auto r = recognize_deals({claim(0xB2, kWork), claim(0xC3, kWork)});
    EXPECT_TRUE(r.recognized.empty());
    EXPECT_EQ(r.oversold, Hashes{h(kWork)});
}

// Работы судятся порознь; один и тот же блок, увиденный дважды, — одна сделка.
TEST(DealRecognition, WorksAreIndependentAndDuplicatesAreOneDeal) {
    const auto r = recognize_deals({claim(0xB2, kWork), claim(0xB2, kWork),
                                    claim(0xC3, kOther), claim(0xD4, kOther)});
    EXPECT_EQ(r.recognized, Hashes{h(0xB2)});
    EXPECT_EQ(r.oversold, Hashes{h(kOther)});
}

TEST(DealRecognition, SettledExactly) {
    EXPECT_TRUE(settled_exactly(6.23625, 6.23625));
    EXPECT_TRUE(settled_exactly(6.0 + 5e-7, 6.0));    // округление, не другая сделка
    EXPECT_FALSE(settled_exactly(6.0, 6.23625));      // труд оплачен, перенос — нет
    EXPECT_FALSE(settled_exactly(7.0, 6.0));          // переплата — тоже не «=»
    EXPECT_FALSE(settled_exactly(0.0, 0.0));          // из рук в руки ничего не ушло
}
