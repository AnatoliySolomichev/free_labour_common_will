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

// Работа вне нити труда или на часах, которые заявила и другая работа цепи
// (records.md §9.4 v4): продавец сделку адресовал, часы ушли — но в счёт она не
// идёт, ведь эти часы могли продать дважды.
TEST(DealRecognition, ADealOnAnUnsoundWorkMovesHoursButDoesNotCount) {
    DealClaim c = claim(0xA1, kWork, kVera);
    c.sound = false;
    const auto r = recognize_deals({c, claim(0xE1, kWork, kMallory, 900, /*addressed=*/false)});
    EXPECT_TRUE(r.recognized.empty());
    EXPECT_EQ(r.unsound, Hashes{h(0xA1)});
    EXPECT_EQ(r.unsound_results, Hashes{h(kWork)});
    EXPECT_TRUE(r.moves_hours(h(0xA1)));
    EXPECT_EQ(r.unaddressed, Hashes{h(0xE1)});                 // ничтожная остаётся ничтожной
}

// Порция партии — свой результат (records.md §9.8 v2): три покупателя хлеба —
// три признанные сделки; работа, проданная и целиком, и порциями, — двойная
// продажа; порция, чьё звено не сходится, — оплата без статистики.
TEST(DealRecognition, PortionsAreResultsOfTheirOwn) {
    const auto portion = [](uint8_t deal, uint8_t payer, uint8_t release) {
        DealClaim c = claim(deal, kWork, payer);
        c.portion = h(release);
        return c;
    };
    auto r = recognize_deals({portion(0xA1, kVera, 0x71), portion(0xA2, kBoris, 0x72),
                              portion(0xA3, kMallory, 0x73)});
    EXPECT_EQ(r.recognized, (Hashes{h(0xA1), h(0xA2), h(0xA3)}));

    r = recognize_deals({portion(0xA1, kVera, 0x71), claim(0xB1, kWork, kBoris)});
    EXPECT_TRUE(r.recognized.empty());
    EXPECT_EQ(r.oversold, Hashes{h(kWork)});
    EXPECT_EQ(r.contested, (Hashes{h(0xA1), h(0xB1)}));

    DealClaim off = portion(0xA1, kVera, 0x71);
    off.sound = false;                                         // звено сверх партии
    r = recognize_deals({off, portion(0xA2, kBoris, 0x72)});
    EXPECT_EQ(r.recognized, Hashes{h(0xA2)});
    EXPECT_EQ(r.unsound, Hashes{h(0xA1)});
    EXPECT_EQ(r.unsound_results, Hashes{h(0x71)});
}

// Перепродажа (records.md §9.8 v4): покупка у держателя — свой результат из его
// запаса; часы уходят, но в статистику труда она не входит.
TEST(DealRecognition, AResaleMovesHoursAndCountsInNoStatistic) {
    DealClaim c = claim(0xA1, 0x70, kVera);                    // work — покупка держателя
    c.portion = h(0x57);
    c.resale  = true;
    const auto r = recognize_deals({c});
    EXPECT_TRUE(r.recognized.empty());
    EXPECT_EQ(r.resold, Hashes{h(0xA1)});
    EXPECT_TRUE(r.moves_hours(h(0xA1)));
}

TEST(DealRecognition, SettledExactly) {
    EXPECT_TRUE(settled_exactly(6.23625, 6.23625));
    EXPECT_TRUE(settled_exactly(6.0 + 5e-7, 6.0));    // округление, не другая сделка
    EXPECT_FALSE(settled_exactly(6.0, 6.23625));      // труд оплачен, перенос — нет
    EXPECT_FALSE(settled_exactly(7.0, 6.0));          // переплата — тоже не «=»
    EXPECT_FALSE(settled_exactly(0.0, 0.0));          // из рук в руки ничего не ушло
}

// v6 (economy.md §4.4): адресованная сделка, которую продавец ещё не принял,
// отверг или пропустил, ничего не двигает — и не мешает той, что её заменит.
TEST(DealRecognition, AnUnsettledOfferStandsInNobodysWay) {
    DealClaim refused = claim(0x01, kWork, kVera, 1000);
    refused.settled = false;
    const auto r = recognize_deals({refused, claim(0x02, kWork, kVera, 2000)});
    EXPECT_EQ(r.unsettled, (Hashes{h(0x01)}));
    EXPECT_EQ(r.recognized, (Hashes{h(0x02)}));   // не «повтор»: первая не состоялась
    EXPECT_TRUE(r.repeated.empty());
    EXPECT_FALSE(r.moves_hours(h(0x01)));
    EXPECT_TRUE(r.moves_hours(h(0x02)));
}
