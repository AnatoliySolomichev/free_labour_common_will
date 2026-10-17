// When a deal happens and what pays it (economy.md §4.4, records.md §11.13):
// the seller's answer by the term, and the thread of a bill in the holder's
// purse.

#include <records/settlement.h>

#include <gtest/gtest.h>

using namespace records;

namespace {

std::array<uint8_t, 32> uid(uint8_t fill) {
    std::array<uint8_t, 32> a{};
    a.fill(fill);
    return a;
}

Ref ref(uint8_t chain, uint8_t hash) {
    Ref r{};
    r.chain.fill(chain);
    r.hash.fill(hash);
    return r;
}

const auto BUYER  = uid(0xB1);
const auto SELLER = uid(0x5E);
constexpr uint32_t PURSE = 7;

// Борис (покупатель) платит Анне (продавцу) в её кошелёк — ветку PURSE — до срока.
Transfer offer(int64_t ts = 100, int64_t until = 200) {
    Transfer t{};
    t.from      = BUYER;
    t.to        = SELLER;
    t.to_node   = PURSE;
    t.origins   = {{BUYER, 3.0}};
    t.timestamp = ts;
    t.until     = until;
    return t;
}

ObservedReply reply(const Ref& deal, bool accepted, int64_t ts, uint64_t index,
                    const std::array<uint8_t, 32>& author = SELLER, uint32_t node = PURSE) {
    ObservedReply r{};
    r.reply.deal      = deal;
    r.reply.accepted  = accepted;
    r.reply.timestamp = ts;
    r.author          = author;
    r.node            = node;
    r.index           = index;
    return r;
}

} // namespace

// ── Ответ продавца ───────────────────────────────────────────────────────────

TEST(DealAnswer, AcceptedOrRefusedByTheSellerInTime) {
    const auto deal = ref(0xB1, 0x70);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, true, 150, 3)}).answer, Answer::accepted);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, false, 150, 3)}).answer, Answer::refused);
    EXPECT_EQ(answer_of(offer(), deal, {}).answer, Answer::none);
    const auto a = answer_of(offer(), deal, {reply(ref(0xB1, 0x71), true, 150, 2),
                                             reply(deal, true, 150, 3)});
    ASSERT_TRUE(a.reply.has_value());
    EXPECT_EQ(*a.reply, 1u);                                  // какой ответ засчитан
}

// Отвечает только кошелёк, в который платят: не посторонний и не другая ветка.
TEST(DealAnswer, OnlyThePurseBeingPaidAnswers) {
    const auto deal = ref(0xB1, 0x70);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, true, 150, 3, uid(0xEE))}).answer,
              Answer::none);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, true, 150, 3, SELLER, PURSE + 1)}).answer,
              Answer::none);
}

// Первое слово окончательно: ветка — линия, «первое» не требует часов.
TEST(DealAnswer, TheFirstWordIsFinal) {
    const auto deal = ref(0xB1, 0x70);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, false, 160, 9), reply(deal, true, 150, 4)})
                  .answer,
              Answer::accepted);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, true, 160, 9), reply(deal, false, 150, 4)})
                  .answer,
              Answer::refused);
}

// После срока оферта мертва: позднее слово ничего не отвечает.
TEST(DealAnswer, AWordPastTheTermAnswersNothing) {
    const auto deal = ref(0xB1, 0x70);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, true, 200, 3)}).answer, Answer::accepted);
    EXPECT_EQ(answer_of(offer(), deal, {reply(deal, true, 201, 3)}).answer, Answer::none);
}

TEST(DealAnswer, StatusAtAMoment) {
    const Transfer t = offer(100, 200);
    EXPECT_EQ(deal_status(t, Answer::none, 150), DealStatus::pending);
    EXPECT_EQ(deal_status(t, Answer::none, 201), DealStatus::expired);
    EXPECT_EQ(deal_status(t, Answer::accepted, 999), DealStatus::accepted);
    EXPECT_EQ(deal_status(t, Answer::refused, 150), DealStatus::refused);
    EXPECT_STREQ(deal_status_name(DealStatus::pending), "pending");
    EXPECT_STREQ(deal_status_name(DealStatus::accepted), "accepted");
    EXPECT_STREQ(deal_status_name(DealStatus::refused), "refused");
    EXPECT_STREQ(deal_status_name(DealStatus::expired), "expired");
}

// Выкуп по заранее подписанному возврату не ждёт ответа, если платит бумагой,
// на которую держатель согласился, подписав возврат: своей и адресата.
TEST(DealAnswer, ABuybackPaidWithAgreedPaperNeedsNoAnswer) {
    Transfer t = offer();                                      // адресат BUYER платит держателю SELLER
    t.origins = {{BUYER, 2.0}, {SELLER, 1.0, ref(0x77, 0x70), std::nullopt}};
    EXPECT_TRUE(pre_accepted_buyback(t));
    t.origins.push_back({uid(0xC3), 1.0, ref(0x78, 0x70), std::nullopt});
    EXPECT_FALSE(pre_accepted_buyback(t));                     // чужая бумага — ждёт ответа
    t.origins.clear();
    EXPECT_FALSE(pre_accepted_buyback(t));
}

// ── Векселя ──────────────────────────────────────────────────────────────────

TEST(Bill, CreditIsWhatTheBlockPutInTheHolderHands) {
    Transfer src = offer();
    src.origins = {{BUYER, 2.0}, {uid(0xC3), 1.5, ref(0x01, 0x02), std::nullopt},
                   {uid(0xC3), 0.5, ref(0x01, 0x03), std::nullopt}};
    EXPECT_DOUBLE_EQ(bill_credit(src, uid(0xC3)), 2.0);
    EXPECT_DOUBLE_EQ(bill_credit(src, BUYER), 2.0);
    EXPECT_DOUBLE_EQ(bill_credit(src, uid(0xEE)), 0.0);
}

TEST(Bill, AReceiptNamingOwnDealIsAReversal) {
    Redemption rd{};
    rd.transfer = ref(0xB1, 0x70);
    EXPECT_TRUE(is_reversal(rd, BUYER));                       // своя сделка не состоялась
    EXPECT_FALSE(is_reversal(rd, SELLER));                     // чужой перевод вернул бумагу
}

namespace {

const auto HOLDER = uid(0xA1);

BillSpend spend(uint8_t hash, uint64_t index, double units,
                std::optional<uint8_t> prev = std::nullopt, int64_t ts = 0) {
    BillSpend s{};
    s.hash.fill(hash);
    s.node      = PURSE;
    s.index     = index;
    s.timestamp = ts ? ts : static_cast<int64_t>(index) * 10;
    s.units     = units;
    s.until     = s.timestamp + 1000;
    s.answer    = Answer::accepted;
    if (prev) s.prev = ref(0xA1, *prev);
    return s;
}

} // namespace

// Чистая нить: каждая трата называет предыдущую и умещается в остаток.
TEST(Bill, ACleanThreadSpendsWithinTheCredit) {
    const auto st = check_bill(HOLDER, PURSE, 5.0, 2,
                               {spend(0x11, 4, 2.0), spend(0x12, 6, 3.0, 0x11)});
    EXPECT_EQ(st.backed.size(), 2u);
    EXPECT_TRUE(st.unbacked.empty());
    EXPECT_DOUBLE_EQ(st.consumed, 5.0);
    EXPECT_DOUBLE_EQ(st.remaining(), 0.0);
}

// Трата сверх остатка — не бумага: с этого звена нить порвана.
TEST(Bill, ASpendPastTheCreditIsNotPaper) {
    const auto st = check_bill(HOLDER, PURSE, 5.0, 2,
                               {spend(0x11, 4, 2.0), spend(0x12, 6, 3.5, 0x11),
                                spend(0x13, 8, 0.5, 0x12)});
    EXPECT_EQ(st.backed.size(), 1u);
    EXPECT_EQ(st.unbacked.size(), 2u);                         // и всё после разрыва
    EXPECT_EQ(st.fault, "трата больше остатка векселя");
}

// Бумага, которой не было: ни вексель, ни нить не сходятся.
TEST(Bill, BreaksAreCaught) {
    // из другой ветки
    BillSpend other = spend(0x11, 4, 1.0);
    other.node = PURSE + 1;
    EXPECT_EQ(check_bill(HOLDER, PURSE, 5.0, 2, {other}).unbacked.size(), 1u);
    // раньше, чем принят
    EXPECT_EQ(check_bill(HOLDER, PURSE, 5.0, 4, {spend(0x11, 4, 1.0)}).unbacked.size(), 1u);
    // первая трата называет предыдущую
    EXPECT_EQ(check_bill(HOLDER, PURSE, 5.0, 2, {spend(0x11, 4, 1.0, 0x10)}).unbacked.size(), 1u);
    // скрытая трата: вторая называет не первую
    const auto hidden = check_bill(HOLDER, PURSE, 5.0, 2,
                                   {spend(0x11, 4, 1.0), spend(0x12, 6, 1.0, 0x10)});
    EXPECT_EQ(hidden.unbacked.size(), 1u);
    EXPECT_EQ(hidden.fault, "нить векселя порвана: трата называет не предыдущую трату");
    // предыдущая трата чужой цепи
    BillSpend foreign = spend(0x12, 6, 1.0, 0x11);
    foreign.prev->chain = uid(0xEE);
    EXPECT_EQ(check_bill(HOLDER, PURSE, 5.0, 2, {spend(0x11, 4, 1.0), foreign}).unbacked.size(),
              1u);
}

// Отказанная оферта ничего не держала: её часы снова у держателя.
TEST(Bill, ARefusedOfferFreesItsHours) {
    BillSpend refused = spend(0x11, 4, 5.0);
    refused.answer = Answer::refused;
    const auto st = check_bill(HOLDER, PURSE, 5.0, 2, {refused, spend(0x12, 6, 5.0, 0x11)});
    EXPECT_EQ(st.backed.size(), 2u);
    EXPECT_DOUBLE_EQ(st.consumed, 5.0);
}

// Оферта без ответа держит часы, пока не вышел её срок: заплатить ими снова можно
// только после срока.
TEST(Bill, AnUnansweredOfferHoldsItsHoursUntilItsTerm) {
    BillSpend waiting = spend(0x11, 4, 5.0);
    waiting.answer = Answer::none;
    waiting.until  = 100;
    const auto after = check_bill(HOLDER, PURSE, 5.0, 2,
                                  {waiting, spend(0x12, 6, 5.0, 0x11, 150)});
    EXPECT_EQ(after.backed.size(), 2u);                        // срок вышел — снова свои
    const auto before = check_bill(HOLDER, PURSE, 5.0, 2,
                                   {waiting, spend(0x12, 6, 5.0, 0x11, 90)});
    EXPECT_EQ(before.unbacked.size(), 1u);                     // ещё держит — двойная трата
    const auto alone = check_bill(HOLDER, PURSE, 5.0, 2, {waiting});
    EXPECT_DOUBLE_EQ(alone.consumed, 5.0);                     // последняя — держит
}

// Перемещение между своими ветками стоит всегда, а один блок — одна трата.
TEST(Bill, AMoveAlwaysStandsAndABlockCountsOnce) {
    BillSpend move = spend(0x11, 4, 2.0);
    move.move   = true;
    move.answer = Answer::none;
    move.until  = 0;
    const auto st = check_bill(HOLDER, PURSE, 5.0, std::nullopt,
                               {move, move, spend(0x12, 6, 3.0, 0x11, 9999)});
    EXPECT_EQ(st.backed.size(), 2u);
    EXPECT_DOUBLE_EQ(st.consumed, 5.0);
}

// Трата, которая ничего не сдвинула — сделка не по адресу или с чужой бумагой без
// векселя, — часов не держит, даже если её «приняли».
TEST(Bill, ASpendThatMovesNothingHoldsNothing) {
    BillSpend void_deal = spend(0x11, 4, 5.0);
    void_deal.stands = false;
    const auto st = check_bill(HOLDER, PURSE, 5.0, 2, {void_deal, spend(0x12, 6, 5.0, 0x11)});
    EXPECT_EQ(st.backed.size(), 2u);
    EXPECT_DOUBLE_EQ(st.consumed, 5.0);
}

// Пошагово — то же правило: трата судится по тратам до неё, а как она кончилась
// (держит ли часы), читатель сообщает, когда узнаёт.
TEST(Bill, TheRuleSpendBySpendIsCheckBill) {
    BillRun run(HOLDER, PURSE, 5.0, 2);
    const BillSpend a = spend(0x11, 4, 3.0);
    EXPECT_TRUE(run.judge(a));
    BillSpend refused = a;
    refused.answer    = Answer::refused;
    run.settle(refused, std::nullopt);                      // отвергнута — часов не держит
    EXPECT_DOUBLE_EQ(run.state().remaining(), 5.0);
    const BillSpend b = spend(0x12, 6, 5.0, 0x11);
    EXPECT_TRUE(run.judge(b));                               // весь вексель снова свободен
    run.settle(b, std::nullopt);
    EXPECT_DOUBLE_EQ(run.state().remaining(), 0.0);
    EXPECT_FALSE(run.judge(spend(0x13, 8, 0.5, 0x12)));      // сверх остатка
    EXPECT_FALSE(run.judge(spend(0x14, 9, 0.1, 0x13)));      // и всё после разрыва
    EXPECT_EQ(run.state().unbacked.size(), 2u);
}

// ── Залог и время ответа ─────────────────────────────────────────────────────

// Ответ не раньше блоков перед ним в ветке: «принял» задним числом после блока,
// датированного позже срока, — поздно (economy.md §8.14).
TEST(DealAnswer, AnAnswerIsNoEarlierThanItsBranch) {
    const auto deal = ref(0xB1, 0x70);
    auto late = reply(deal, true, 150, 3);
    late.since = 250;                                          // ветка уже прошла срок
    EXPECT_EQ(answer_of(offer(), deal, {late}).answer, Answer::none);
    late.since = 180;
    EXPECT_EQ(answer_of(offer(), deal, {late}).answer, Answer::accepted);
}

// Выкуп залогом: согласована сама оплата покупки и своя бумага держателя; своя
// эмиссия адресата — уже нет.
TEST(DealAnswer, ADepositBuybackIsPaidWithTheDeposit) {
    Transfer purchase = offer();                               // держатель BUYER купил у SELLER
    Acceptance a{};
    a.buyback      = Buyback{SELLER, 900, true, true};
    purchase.accept = a;
    const Ref p = ref(0xB1, 0x70);
    Transfer back{};                                           // SELLER выкупает у BUYER
    back.from    = SELLER;
    back.to      = BUYER;
    back.origins = {{uid(0xC3), 2.0, p, std::nullopt}, {BUYER, 1.0, ref(0x77, 0x01), std::nullopt}};
    EXPECT_TRUE(pre_accepted_buyback(back, p, purchase));
    back.origins.push_back({SELLER, 1.0});                     // своя эмиссия адресата
    EXPECT_FALSE(pre_accepted_buyback(back, p, purchase));
    // Часть оплаты была его же бумагой — она умерла у него с долгом, залогом не
    // стала; ею же он и возвращает, но не больше, чем её было.
    purchase.origins.push_back({SELLER, 1.0});
    EXPECT_TRUE(pre_accepted_buyback(back, p, purchase));
    back.origins.back().units = 1.5;
    EXPECT_FALSE(pre_accepted_buyback(back, p, purchase));
    purchase.accept->buyback->deposit = false;                 // без залога — правило А
    EXPECT_TRUE(pre_accepted_buyback(back, p, purchase) == pre_accepted_buyback(back));
}

TEST(Bill, ADepositIsFrozenUntilTheBuybackOrTheTerm) {
    Transfer purchase = offer();
    Acceptance a{};
    a.buyback       = Buyback{SELLER, 900, false, true};
    purchase.accept = a;
    EXPECT_TRUE(deposit_frozen(purchase, 500, false));         // до срока, выкупа не было
    EXPECT_FALSE(deposit_frozen(purchase, 500, true));         // выкуп — первым
    EXPECT_FALSE(deposit_frozen(purchase, 901, false));        // срок прошёл
    purchase.accept->buyback->deposit = false;
    EXPECT_FALSE(deposit_frozen(purchase, 500, false));
    BillSpend s = spend(0x11, 4, 1.0);
    s.frozen = true;
    const auto st = check_bill(HOLDER, PURSE, 5.0, 2, {s, spend(0x12, 6, 1.0, 0x11)});
    EXPECT_EQ(st.unbacked.size(), 2u);
    EXPECT_EQ(st.fault, "залог заморожен до срока возврата: первым его тратит выкуп");
}
