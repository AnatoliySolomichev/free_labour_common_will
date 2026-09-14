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

// A deal on `work` by `payer`, as the recognizer sees it; addressed unless said.
DealClaim claim(uint8_t deal, uint8_t work, uint8_t payer, int64_t ts = 1000,
                bool addressed = true) {
    return DealClaim{h(deal), h(work), h(payer), ts, addressed};
}

constexpr uint8_t kWork = 0x10, kOther = 0x20;
constexpr uint8_t kVera = 0xB2, kBoris = 0xC3, kMallory = 0xEE;
constexpr uint8_t kAnna = 0xA1;   // the worker — the seller

// Anna's work, bought by `payer`: a deal as it is written.
Transfer deal_by(uint8_t payer, int64_t ts) {
    Acceptance a{};
    a.work.chain  = h(kAnna);
    a.work.hash   = h(kWork);
    a.hours_raw   = 2;
    a.labor_units = 2;
    Transfer t{};
    t.from      = h(payer);
    t.to        = h(kAnna);
    t.origins   = {{h(payer), 2.0}};
    t.accept    = a;
    t.timestamp = ts;
    return t;
}

Release release_to(uint8_t buyer, int64_t expires, uint8_t work = kWork) {
    Release rl{};
    rl.work.chain = h(kAnna);
    rl.work.hash  = h(work);
    rl.to         = h(buyer);
    rl.timestamp  = 500;
    rl.expires    = expires;
    return rl;
}

} // namespace

// ── Кому достаётся результат: решает продавец (records.md §9.5) ──────────────

// Заказчик в записи труда — сделка его, отпуск не нужен; чужая — не по адресу.
TEST(Addressing, TheCustomerOfTheWorkIsAddressed) {
    const std::optional<std::array<uint8_t, 32>> vera = h(kVera);
    EXPECT_TRUE(addressed_to(deal_by(kVera, 1000), vera, nullptr));
    EXPECT_FALSE(addressed_to(deal_by(kBoris, 1000), vera, nullptr));
    EXPECT_FALSE(addressed_to(deal_by(kVera, 1000), std::nullopt, nullptr));   // никого не называли
}

// Отпуск называет покупателя и срок: засчитывается сделка названного — до срока.
TEST(Addressing, ALiveReleaseToThePayerIsAddressed) {
    const Release rl = release_to(kVera, 2000);
    auto t = deal_by(kVera, 1999);
    t.accept->release = Ref{h(kAnna), h(0x77)};
    EXPECT_TRUE(addressed_to(t, std::nullopt, &rl));

    t.timestamp = 2000;                                 // в сам срок — уже поздно
    EXPECT_FALSE(addressed_to(t, std::nullopt, &rl));

    auto boris = deal_by(kBoris, 1500);                 // отпуск не на него
    boris.accept->release = Ref{h(kAnna), h(0x77)};
    EXPECT_FALSE(addressed_to(boris, std::nullopt, &rl));

    const Release other = release_to(kVera, 2000, kOther);   // отпуск другой работы
    EXPECT_FALSE(addressed_to(t, std::nullopt, &other));
}

// Сделка, назвавшая отпуск, судится только им: заказчиком она себя не спасёт, а
// отпуск, которого нет на столе, — не адрес.
TEST(Addressing, ADealNamingAReleaseIsJudgedByItAlone) {
    auto t = deal_by(kVera, 1000);
    t.accept->release = Ref{h(kAnna), h(0x77)};
    EXPECT_FALSE(addressed_to(t, h(kVera), nullptr));
}

// ── Какая сделка считается (records.md §9.5) ─────────────────────────────────

// Одна адресованная сделка — она и считается.
TEST(DealRecognition, LoneAddressedDealCounts) {
    const auto r = recognize_deals({claim(0xA1, kWork, kVera)});
    EXPECT_EQ(r.recognized, Hashes{h(0xA1)});
    EXPECT_TRUE(r.moves_hours(h(0xA1)));
    EXPECT_TRUE(r.oversold.empty());
}

// Сделка не по адресу ничтожна: ни приёмки, ни оплаты — и честную она не трогает.
// Это и было дырой прежнего правила: посторонний, готовый заплатить, губил
// честную сделку задним числом.
TEST(DealRecognition, AStrangerCannotSpoilAnHonestDeal) {
    const auto r = recognize_deals({claim(0xA1, kWork, kVera),
                                    claim(0xE1, kWork, kMallory, 900, /*addressed=*/false)});
    EXPECT_EQ(r.recognized, Hashes{h(0xA1)});           // сделка Веры стоит
    EXPECT_EQ(r.unaddressed, Hashes{h(0xE1)});
    EXPECT_FALSE(r.moves_hours(h(0xE1)));               // часы Мэллори никуда не ушли
    EXPECT_TRUE(r.oversold.empty());
}

// Один покупатель заплатил дважды — считается первая, вторая — повтор: часы ушли,
// в статистику второй раз ничего не входит.
TEST(DealRecognition, TheSameBuyerPayingAgainIsARepeat) {
    const auto r = recognize_deals({claim(0xB2, kWork, kVera, 2000),
                                    claim(0xB1, kWork, kVera, 1000)});
    EXPECT_EQ(r.recognized, Hashes{h(0xB1)});           // раньше по своему времени
    EXPECT_EQ(r.repeated, Hashes{h(0xB2)});
    EXPECT_TRUE(r.moves_hours(h(0xB2)));
    EXPECT_TRUE(r.oversold.empty());
}

// Продавец отдал один результат двоим — двойная продажа: не считается ни одна,
// и оба адреса подписаны им самим.
TEST(DealRecognition, TwoAddressedBuyersAreTheSellersDoubleSale) {
    const auto r = recognize_deals({claim(0xB2, kWork, kVera), claim(0xC3, kWork, kBoris)});
    EXPECT_TRUE(r.recognized.empty());
    EXPECT_EQ(r.oversold, Hashes{h(kWork)});
    EXPECT_EQ(r.contested, (Hashes{h(0xB2), h(0xC3)}));
    EXPECT_TRUE(r.moves_hours(h(0xB2)));                // часы обоих ушли — видно, кому
    EXPECT_TRUE(r.moves_hours(h(0xC3)));
}

// Работы судятся порознь; один и тот же блок, увиденный дважды, — одна сделка.
TEST(DealRecognition, WorksAreIndependentAndDuplicatesAreOneDeal) {
    const auto r = recognize_deals({claim(0xB2, kWork, kVera), claim(0xB2, kWork, kVera),
                                    claim(0xC3, kOther, kVera), claim(0xD4, kOther, kBoris)});
    EXPECT_EQ(r.recognized, Hashes{h(0xB2)});
    EXPECT_TRUE(r.repeated.empty());
    EXPECT_EQ(r.oversold, Hashes{h(kOther)});
}

TEST(DealRecognition, SettledExactly) {
    EXPECT_TRUE(settled_exactly(6.23625, 6.23625));
    EXPECT_TRUE(settled_exactly(6.0 + 5e-7, 6.0));    // округление, не другая сделка
    EXPECT_FALSE(settled_exactly(6.0, 6.23625));      // труд оплачен, перенос — нет
    EXPECT_FALSE(settled_exactly(7.0, 6.0));          // переплата — тоже не «=»
    EXPECT_FALSE(settled_exactly(0.0, 0.0));          // из рук в руки ничего не ушло
}
