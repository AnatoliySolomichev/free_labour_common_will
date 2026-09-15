#include "records/goods_thread.h"

#include <gtest/gtest.h>

using namespace records;

namespace {

using Hash32 = std::array<uint8_t, 32>;
using Hashes = std::set<Hash32>;

Hash32 h(uint8_t fill) {
    Hash32 x{};
    x.fill(fill);
    return x;
}

const Hash32 kChain = h(0xA1);
constexpr int64_t kHour = 3600;

// A link of kChain's thread, from hour `from` to hour `to`.
ObservedWork link(uint8_t hash, int64_t from, int64_t to, uint64_t seq,
                  std::vector<uint8_t> prev) {
    ObservedWork w{};
    w.hash     = h(hash);
    w.interval = {from * kHour, to * kHour};
    ThreadLink l{};
    l.seq = seq;
    for (const auto p : prev) l.prev.push_back(Ref{kChain, h(p)});
    w.link = l;
    return w;
}

// A work written outside the thread.
ObservedWork loose(uint8_t hash, int64_t from, int64_t to) {
    ObservedWork w{};
    w.hash     = h(hash);
    w.interval = {from * kHour, to * kHour};
    return w;
}

} // namespace

TEST(LaborThread, TheIntervalOfAWork) {
    WorkRecord wr{};
    wr.start_ts = 1000;
    wr.hours    = 1.5;
    EXPECT_EQ(labor_interval(wr), (std::pair<int64_t, int64_t>{1000, 1000 + 5400}));
    EXPECT_TRUE(intervals_cross({0, 10}, {9, 20}));
    EXPECT_FALSE(intervals_cross({0, 10}, {10, 20}));   // конец одной — начало другой
}

// Нить по порядку: всё в нити, пересечений нет, вершина одна.
TEST(LaborThread, AStraightThreadIsSound) {
    const auto t = check_labor_thread(kChain, {link(1, 0, 8, 0, {}),
                                               link(2, 8, 16, 1, {1}),
                                               link(3, 24, 30, 2, {2})});
    for (uint8_t i = 1; i <= 3; ++i) EXPECT_TRUE(t.sound(h(i)));
    EXPECT_EQ(t.tips, std::vector<Hash32>{h(3)});
    EXPECT_EQ(t.forks, 0u);
    EXPECT_FALSE(t.gaps);
}

// Форк без пересечения — не обман (правило форка, ИР-023): понедельник записан на
// телефоне, вторник на компьютере; звено с двумя предыдущими сводит ветки.
TEST(LaborThread, AForkWithoutCrossingIsNoFraud) {
    const auto t = check_labor_thread(kChain, {link(1, 0, 8, 0, {}),
                                               link(2, 24, 32, 1, {1}),
                                               link(3, 48, 56, 1, {1}),
                                               link(4, 72, 80, 2, {2, 3})});
    for (uint8_t i = 1; i <= 4; ++i) EXPECT_TRUE(t.sound(h(i)));
    EXPECT_EQ(t.forks, 1u);
    EXPECT_EQ(t.tips, std::vector<Hash32>{h(4)});
}

// Одни и те же часы в двух работах — подписанное доказательство. Какая из двух
// фиктивна, не узнать, поэтому не в счёт обе; стык в 10:00 пересечением не считается.
TEST(LaborThread, CrossingHoursSpoilBothWorks) {
    const auto t = check_labor_thread(kChain, {link(1, 0, 8, 0, {}),
                                               link(2, 6, 10, 1, {1}),
                                               link(3, 10, 12, 2, {2})});
    EXPECT_FALSE(t.sound(h(1)));
    EXPECT_FALSE(t.sound(h(2)));
    EXPECT_TRUE(t.sound(h(3)));
    ASSERT_EQ(t.overlaps.size(), 1u);
    EXPECT_EQ(t.overlaps[0], (std::pair<Hash32, Hash32>{h(1), h(2)}));
}

// Форк с пересечением — обман, и доказательство подписано самой цепью.
TEST(LaborThread, AForkThatCrossesIsCaught) {
    const auto t = check_labor_thread(kChain, {link(1, 0, 8, 0, {}),
                                               link(2, 9, 17, 1, {1}),
                                               link(3, 12, 20, 1, {1})});
    EXPECT_TRUE(t.sound(h(1)));
    EXPECT_FALSE(t.sound(h(2)));
    EXPECT_FALSE(t.sound(h(3)));
    EXPECT_EQ(t.forks, 1u);
}

// Работа вне нити к сделке не признаётся — но её часы всё равно заявлены:
// пересечение с ней портит и звено нити.
TEST(LaborThread, AWorkOutsideTheThreadStillClaimsItsHours) {
    const auto t = check_labor_thread(kChain, {link(1, 0, 8, 0, {}),
                                               loose(2, 20, 24),
                                               loose(3, 4, 6)});
    EXPECT_EQ(t.unthreaded, (Hashes{h(2), h(3)}));
    EXPECT_FALSE(t.sound(h(2)));
    EXPECT_FALSE(t.sound(h(1)));                      // 4–6 внутри 0–8
    EXPECT_EQ(t.overlapping, (Hashes{h(1), h(3)}));
}

// Битое звено — вне нити: номер не на единицу больше предыдущего; предыдущее из
// чужой цепи; предыдущее — не звено. Предыдущего нет на столе — это пробел, а не
// вина: звено остаётся в нити.
TEST(LaborThread, BrokenLinksAreOutOfTheThreadAndGapsAreNotFaults) {
    ObservedWork foreign = link(4, 40, 41, 1, {});
    foreign.link->prev = {Ref{h(0xEE), h(1)}};
    const auto t = check_labor_thread(kChain, {link(1, 0, 8, 0, {}),
                                               link(2, 10, 11, 5, {1}),   // seq не тот
                                               foreign,
                                               loose(9, 30, 31),
                                               link(5, 50, 51, 1, {9}),   // prev — не звено
                                               link(6, 60, 61, 3, {7})}); // prev не получен
    EXPECT_EQ(t.broken, (Hashes{h(2), h(4), h(5)}));
    EXPECT_TRUE(t.sound(h(1)));
    EXPECT_TRUE(t.sound(h(6)));
    EXPECT_TRUE(t.gaps);
    EXPECT_EQ(t.tips, (std::vector<Hash32>{h(1), h(6)}));   // битое 2 продолжением не считается
}

// Работа без часов ничего не заявляет и ни с чем не пересекается.
TEST(LaborThread, AWorkOfNoHoursCrossesNothing) {
    const auto t = check_labor_thread(kChain, {link(1, 0, 8, 0, {}), link(2, 4, 4, 1, {1})});
    EXPECT_TRUE(t.sound(h(1)));
    EXPECT_TRUE(t.sound(h(2)));
}

// ── Партии (records.md §9.8 v2, records.md §9.9) ─────────────────────────────

namespace {

// A link of lot 0 of kChain: its seq, qty and after, the previous link by hash
// byte; a release carries its term, a write-off does not.
ObservedLotLink lot(uint8_t hash, uint64_t seq, double qty, double after,
                    std::optional<uint8_t> prev, std::optional<int64_t> expires = std::nullopt,
                    std::vector<uint8_t> returns = {}, int64_t ts = 1000, bool taken = false) {
    ObservedLotLink l{};
    l.hash       = h(hash);
    l.link.qty   = qty;
    l.link.seq   = seq;
    l.link.after = after;
    if (prev) l.link.prev = Ref{kChain, h(*prev)};
    for (const auto r : returns) l.link.returns.push_back(Ref{kChain, h(r)});
    l.timestamp = ts;
    l.expires   = expires;
    l.taken     = taken;
    return l;
}

} // namespace

// Хлеб: сто буханок, отпущено 30 и 50, списано 5 — остаток 15, всё сходится.
TEST(Lot, AStraightSubThreadChecksOut) {
    const auto st = check_lot(kChain, 100, {lot(1, 0, 30, 30, {}, 2000),
                                            lot(2, 1, 50, 80, 1, 2000),
                                            lot(3, 2, 5, 85, 2)});
    EXPECT_EQ(st.valid, (Hashes{h(1), h(2), h(3)}));
    EXPECT_DOUBLE_EQ(st.remaining(), 15.0);
    EXPECT_FALSE(st.gaps);
}

// Больше, чем в партии, не отпустить: звено сверх партии не сходится.
TEST(Lot, MoreThanTheLotHoldsIsRefused) {
    const auto st = check_lot(kChain, 100, {lot(1, 0, 90, 90, {}), lot(2, 1, 20, 110, 1)});
    EXPECT_EQ(st.invalid, Hashes{h(2)});
    EXPECT_DOUBLE_EQ(st.remaining(), 10.0);
}

// Два звена с одним номером — две продажи из одного остатка: не признаётся ни одно
// (партия судится строго — решение участника, ИР-023).
TEST(Lot, TwoSalesFromOneRemainderAreBothRefused) {
    const auto st = check_lot(kChain, 100, {lot(1, 0, 60, 60, {}),
                                            lot(2, 1, 40, 100, 1),
                                            lot(3, 1, 40, 100, 1)});
    EXPECT_EQ(st.invalid, (Hashes{h(2), h(3)}));
    EXPECT_EQ(st.equivocated_seqs, std::vector<uint64_t>{1});
    EXPECT_DOUBLE_EQ(st.remaining(), 40.0);
}

// Отпуск, истёкший невзятым, возвращается в запас следующим звеном: 4 + 6 − 4.
TEST(Lot, AnExpiredUntakenReleaseGoesBackToStock) {
    const auto st = check_lot(kChain, 10, {lot(1, 0, 4, 4, {}, 500),
                                           lot(2, 1, 6, 6, 1, std::nullopt, {1}, 1000)});
    EXPECT_EQ(st.valid, (Hashes{h(1), h(2)}));
    EXPECT_DOUBLE_EQ(st.remaining(), 4.0);
}

// Вернуть нельзя отпуск ещё живой, уже взятый покупателем, и списание — у него
// нет срока, возвращать нечего.
TEST(Lot, AFalseReturnIsRefused) {
    auto st = check_lot(kChain, 10, {lot(1, 0, 4, 4, {}, 5000),
                                     lot(2, 1, 6, 6, 1, std::nullopt, {1}, 1000)});
    EXPECT_EQ(st.invalid, Hashes{h(2)});
    st = check_lot(kChain, 10, {lot(1, 0, 4, 4, {}, 500, {}, 400, /*taken=*/true),
                                lot(2, 1, 6, 6, 1, std::nullopt, {1}, 1000)});
    EXPECT_EQ(st.invalid, Hashes{h(2)});
    st = check_lot(kChain, 10, {lot(1, 0, 4, 4, {}),
                                lot(2, 1, 6, 6, 1, std::nullopt, {1}, 1000)});
    EXPECT_EQ(st.invalid, Hashes{h(2)});
}

// Предыдущего звена нет на столе — пробел, а не вина.
TEST(Lot, AGapIsNotAFault) {
    const auto st = check_lot(kChain, 100, {lot(3, 2, 10, 50, 2)});
    EXPECT_EQ(st.valid, Hashes{h(3)});
    EXPECT_TRUE(st.gaps);
    EXPECT_DOUBLE_EQ(st.remaining(), 50.0);
}

// Доля работы в порции: доля выхода × порция / размер выхода.
TEST(Lot, ThePartOfTheWorkAPortionCarries) {
    WorkRecord bread{};
    bread.hours   = 10;
    bread.outputs = {{"хлеб", 100, "шт"}};
    LotLink l{};
    l.qty = 25;
    EXPECT_DOUBLE_EQ(*portion_fraction(bread, l), 0.25);

    WorkRecord saw{};
    saw.outputs = {{"доски", 2.0, "м³", 0.9, std::nullopt}, {"опилки", 0.5, "м³", 0.1, std::nullopt}};
    l.out = 1;
    l.qty = 0.25;
    EXPECT_NEAR(*portion_fraction(saw, l), 0.05, 1e-12);          // половина опилок — 5 % работы
    saw.outputs[0].share.reset();
    saw.outputs[1].share.reset();
    EXPECT_FALSE(portion_fraction(saw, l).has_value());           // без долей не оценить
    l.out = 7;
    EXPECT_FALSE(portion_fraction(bread, l).has_value());         // нет такого выхода
}

// ── Партия со штучным учётом (records.md §9.8 v3, ИР-023, вопрос 10) ─────────

namespace {

ObservedLotLink numbered(uint8_t hash, uint64_t seq, std::vector<std::string> units,
                         double after, std::optional<uint8_t> prev,
                         std::optional<int64_t> expires = std::nullopt,
                         std::vector<uint8_t> returns = {}, int64_t ts = 1000) {
    auto l = lot(hash, seq, static_cast<double>(units.size()), after, prev, expires,
                 std::move(returns), ts);
    l.link.units = std::move(units);
    return l;
}

const std::vector<std::string> kFrames = {"R-1", "R-2", "R-3"};

} // namespace

// Велосипеды с номерами рам: номер продаётся один раз.
TEST(NumberedLot, AUnitIsSoldOnce) {
    const auto st = check_lot(kChain, 3, {numbered(1, 0, {"R-2"}, 1, {}),
                                          numbered(2, 1, {"R-2"}, 2, 1)}, kFrames);
    EXPECT_EQ(st.valid, Hashes{h(1)});
    EXPECT_EQ(st.invalid, Hashes{h(2)});
    EXPECT_EQ(st.held, (std::set<std::string>{"R-2"}));
}

// Истёкший невзятый отпуск вернулся в запас — его номер снова свободен.
TEST(NumberedLot, AReturnFreesTheUnit) {
    const auto st = check_lot(kChain, 3, {numbered(1, 0, {"R-2"}, 1, {}, 500),
                                          numbered(2, 1, {"R-2"}, 1, 1, std::nullopt, {1}, 1000)},
                              kFrames);
    EXPECT_EQ(st.valid, (Hashes{h(1), h(2)}));
    EXPECT_EQ(st.held, (std::set<std::string>{"R-2"}));
}

// Номер, которого в партии нет, порция без номеров из партии с номерами и номера
// у партии без номеров — не звенья.
TEST(NumberedLot, UnitsMustBeTheLotsOwn) {
    auto st = check_lot(kChain, 3, {numbered(1, 0, {"X-9"}, 1, {})}, kFrames);
    EXPECT_EQ(st.invalid, Hashes{h(1)});
    st = check_lot(kChain, 3, {lot(1, 0, 1, 1, {})}, kFrames);
    EXPECT_EQ(st.invalid, Hashes{h(1)});
    st = check_lot(kChain, 3, {numbered(1, 0, {"R-1"}, 1, {})});
    EXPECT_EQ(st.invalid, Hashes{h(1)});
}

// ── Порция по ценнику, «бери или нет» (records.md §9.8 v3, ИР-023, вопрос 11) ─

TEST(PortionTerms, APortionIsSoldOnTheLotsTerms) {
    BlockPath skill{};
    skill.hash.fill(0x01);
    BlockPath speed{};
    speed.hash.fill(0x02);
    WorkRecord bread{};
    bread.hours   = 10;
    bread.outputs = {{"хлеб", 100, "шт", std::nullopt, 12.0}};   // ценник: 12 стч за сто
    bread.axes    = {{speed, 0.25}, {skill, 0.75}};
    CarryEntry ce{};
    ce.carried  = 2.0;                                             // печь: 2 ч на всю партию
    bread.carry = {ce};
    LotLink l{};
    l.qty = 25;

    const auto t = portion_terms(bread, l);
    ASSERT_TRUE(t.has_value());
    EXPECT_DOUBLE_EQ(t->hours_raw, 2.5);
    EXPECT_DOUBLE_EQ(t->labor_units, 3.0);
    EXPECT_DOUBLE_EQ(t->carried_units, 0.5);
    ASSERT_EQ(t->axes.size(), 2u);
    EXPECT_EQ(t->axes[0].axis, skill);                             // по хешу оси
    EXPECT_DOUBLE_EQ(t->axes[0].units, 2.25);
    EXPECT_DOUBLE_EQ(t->axes[0].units + t->axes[1].units, 3.0);

    Acceptance a{};
    a.hours_raw     = t->hours_raw;
    a.labor_units   = t->labor_units;
    a.carried_units = t->carried_units;
    a.axes          = t->axes;
    EXPECT_TRUE(meets_terms(a, *t));
    a.labor_units = 2.0;                                           // поторговался — не по ценнику
    EXPECT_FALSE(meets_terms(a, *t));

    bread.outputs[0].price.reset();                                // без ценника порциями не продают
    EXPECT_FALSE(portion_terms(bread, l).has_value());
}

// ── Перепродажа: купленное — партия покупателя (records.md §9.8 v4) ──────────

TEST(Holding, WhatAPurchasePutsInTheBuyersHands) {
    Transfer deal{};
    deal.origins = {{h(0xB2), 3.0}};                           // заплачено 3 стч

    WorkRecord bread{};
    bread.outputs = {{"хлеб", 100, "шт"}};
    Release portion{};
    LotLink l{};
    l.qty = 25;
    portion.lot = l;
    auto lots = holding_lots(deal, bread, &portion);           // порция — одна партия
    ASSERT_EQ(lots.size(), 1u);
    EXPECT_DOUBLE_EQ(lots[0].qty, 25);
    EXPECT_DOUBLE_EQ(lots[0].cost, 3.0);
    EXPECT_DOUBLE_EQ(resale_price(lots[0], 5), 0.6);           // пятая часть — пятая часть цены

    WorkRecord saw{};
    saw.outputs = {{"доски", 2.0, "м³", 0.9, std::nullopt},
                   {"опилки", 0.5, "м³", 0.1, std::nullopt}};
    lots = holding_lots(deal, saw, nullptr);                   // целиком — по партии на выход
    ASSERT_EQ(lots.size(), 2u);
    EXPECT_DOUBLE_EQ(lots[0].cost, 2.7);
    EXPECT_NEAR(lots[1].cost, 0.3, 1e-12);

    WorkRecord haircut{};                                       // услуга — перепродавать нечего
    EXPECT_TRUE(holding_lots(deal, haircut, nullptr).empty());
}

TEST(Holding, AResaleIsCarriedCostOnly) {
    Acceptance a{};
    a.carried_units = 0.6;
    EXPECT_TRUE(meets_resale(a, 0.6));
    a.carried_units = 0.7;                                      // наценка
    EXPECT_FALSE(meets_resale(a, 0.6));
    a.carried_units = 0.6;
    a.labor_units   = 0.1;                                      // труд в перепродаже
    EXPECT_FALSE(meets_resale(a, 0.6));
}

// ── Выкуп с адресом (records.md §9.8 v5, ИР-023, пункт 8) ────────────────────

TEST(Buyout, APurchaseOnAnAddressedBuyoutPreSignsTheReturn) {
    const Addressed buyout{h(0xA1), 5000};
    Acceptance a{};
    EXPECT_FALSE(honours_buyout(a, 3.0, buyout));                   // без возврата — нет
    a.buyback = Buyback{h(0xA1), 3.0, 5000};
    EXPECT_TRUE(honours_buyout(a, 3.0, buyout));
    a.buyback->units = 3.5;                                          // дороже уплаченного
    EXPECT_FALSE(honours_buyout(a, 3.0, buyout));
    a.buyback = Buyback{h(0xB2), 3.0, 5000};                         // не тому
    EXPECT_FALSE(honours_buyout(a, 3.0, buyout));
}

// Адресат выкупает сам — держатель в сделке не участвует; но только он, до срока
// и по подписанной цене.
TEST(Buyout, TheAddresseeBuysBackOnItsOwn) {
    Transfer purchase{};
    purchase.from = h(0xC3);                                         // держатель
    Acceptance pa{};
    pa.work.hash.fill(0x54);
    pa.buyback = Buyback{h(0xA1), 3.0, 5000};
    purchase.accept = pa;

    Transfer back{};
    back.from      = h(0xA1);                                        // адресат
    back.to        = h(0xC3);
    back.timestamp = 4000;
    Acceptance ba{};
    ba.work          = pa.work;
    ba.carried_units = 3.0;
    back.accept = ba;
    EXPECT_TRUE(completes_buyback(back, purchase));
    back.timestamp = 5000;                                           // срок прошёл
    EXPECT_FALSE(completes_buyback(back, purchase));
    back.timestamp = 4000;
    back.from = h(0xB2);                                             // не адресат
    EXPECT_FALSE(completes_buyback(back, purchase));
    back.from = h(0xA1);
    back.accept->carried_units = 2.0;                                // не по подписанной цене
    EXPECT_FALSE(completes_buyback(back, purchase));
}
