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

AcceptanceClaim claim(uint8_t acceptance, uint8_t work, bool settled) {
    return AcceptanceClaim{h(acceptance), h(work), settled};
}

constexpr uint8_t kWork = 0x10, kOther = 0x20;

} // namespace

// Одна приёмка у работы — считается, оплачена она или нет: квалификация читает
// приёмки и до оплаты (records.md §14.4).
TEST(AcceptanceRecognition, LoneAcceptanceCountsPaidOrNot) {
    auto r = recognize_acceptances({claim(0xA1, kWork, false)});
    EXPECT_EQ(r.recognized, Hashes{h(0xA1)});
    EXPECT_TRUE(r.oversold.empty());

    r = recognize_acceptances({claim(0xA1, kWork, true)});
    EXPECT_EQ(r.recognized, Hashes{h(0xA1)});
}

// Оплаченная важнее неоплаченной: чужая приёмка, за которую никто не заплатил,
// не вытесняет ту, которой работу оплатили, — писать её бесплатно, весит она ноль.
TEST(AcceptanceRecognition, PaidBeatsUnpaid) {
    const auto r = recognize_acceptances({claim(0xEE, kWork, false),
                                          claim(0xB2, kWork, true)});
    EXPECT_EQ(r.recognized, Hashes{h(0xB2)});
    EXPECT_TRUE(r.oversold.empty());
}

// Оплачена дважды — продана дважды: не считается ни одна, и работа видна.
TEST(AcceptanceRecognition, PaidTwiceIsSoldTwice) {
    const auto r = recognize_acceptances({claim(0xB2, kWork, true),
                                          claim(0xC3, kWork, true)});
    EXPECT_TRUE(r.recognized.empty());
    EXPECT_EQ(r.oversold, Hashes{h(kWork)});
}

// Две неоплаченные: предпочесть одну нечем — не считается ни одна, пока одну
// из них не оплатят.
TEST(AcceptanceRecognition, TwoUnpaidCountNothingUntilOneIsPaid) {
    auto r = recognize_acceptances({claim(0xB2, kWork, false), claim(0xC3, kWork, false)});
    EXPECT_TRUE(r.recognized.empty());
    EXPECT_TRUE(r.oversold.empty());

    r = recognize_acceptances({claim(0xB2, kWork, false), claim(0xC3, kWork, true)});
    EXPECT_EQ(r.recognized, Hashes{h(0xC3)});
}

// Работы судятся порознь; один и тот же блок, увиденный дважды, — одна заявка.
TEST(AcceptanceRecognition, WorksAreIndependentAndDuplicatesAreOneClaim) {
    const auto r = recognize_acceptances({claim(0xB2, kWork, true),
                                          claim(0xB2, kWork, true),    // тот же блок
                                          claim(0xC3, kOther, true),
                                          claim(0xD4, kOther, true)});
    EXPECT_EQ(r.recognized, Hashes{h(0xB2)});
    EXPECT_EQ(r.oversold, Hashes{h(kOther)});
}

TEST(AcceptanceRecognition, SettledExactly) {
    EXPECT_TRUE(settled_exactly(6.23625, 6.23625));
    EXPECT_TRUE(settled_exactly(6.0 + 5e-7, 6.0));    // округление, не другая сделка
    EXPECT_FALSE(settled_exactly(6.0, 6.23625));      // труд оплачен, перенос — нет
    EXPECT_FALSE(settled_exactly(7.0, 6.0));          // переплата — тоже не «=»
    EXPECT_FALSE(settled_exactly(0.0, 0.0));          // из рук в руки ничего не ушло
}
