#include "aggregator/rates_view.h"
#include "aggregator/server.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>
#include <httplib.h>

#include <filesystem>
#include <thread>

// Ось — по тегу хеша её определения; тег 0x01 — общая ось тестов. Содержание
// оси здесь не проверяется: важно лишь, что приёмки без разбивки не существует
// (records.md §9.5 v4) и что цена считается по осям (records.md §11.2 v5).
inline records::BlockPath test_axis(uint8_t tag = 0x01) {
    records::BlockPath r{};
    r.chain.fill(0x79);
    r.hash.fill(tag);
    return r;
}

using namespace aggregator;
using namespace blockchain;

namespace {

constexpr int64_t kDay = 86'400LL * 100;   // the tested UTC day

UserId make_chain(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

Block make_record_block(const UserId& owner, BlockIndex index,
                        const records::Record& rec) {
    Block b{};
    b.address           = {owner, 0x7FFF'FFFFu, index};
    b.prev_hash         = Hash::zero();
    b.timestamp_claimed = static_cast<Timestamp>(index) * 1000LL;
    b.type              = BlockType::DATA;
    b.payload           = records::Codec::encode(rec);
    b.signature         = Signature::null();
    const auto bytes    = Serializer::encode(b);
    const KeyPair kp    = Crypto::generate_keypair();
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

records::Ref ref_to(const UserId& chain, const Block& block) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = Crypto::hash_block(block).bytes;
    return r;
}

// Цена оси по тегу в выдаче дня; nullptr — такой оси в выдаче нет.
const records::AxisPrice* price_of(const std::vector<records::AxisPrice>& v,
                                   uint8_t tag) {
    for (const auto& p : v)
        if (p.axis == test_axis(tag)) return &p;
    return nullptr;
}

} // namespace

class RatesViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    UserId alice_ = make_chain(0xA1);   // worker
    UserId bob_   = make_chain(0xB2);   // payer

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_rates_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    Block add(const UserId& owner, const records::Record& rec) {
        const Block b = make_record_block(owner, next_index_++, rec);
        EXPECT_TRUE(storage_->add_block(b));
        return b;
    }

    // One deal: work → the Transfer that carries its acceptance (by `payer`) —
    // appraisal and payment in one record (records.md §9.5, ИР-023).
    // `lines` — the breakdown {axis tag, units}; by default all on the common axis.
    records::Ref settled_deal(const UserId& payer, double hours, double units,
                              std::vector<std::pair<uint8_t, double>> lines = {}) {
        if (lines.empty()) lines = {{0x01, units}};
        records::WorkRecord wr{};
        wr.hours = hours;
        wr.customer = payer.bytes;   // на заказ плательщика (records.md §9.4)
        const Block work = add(alice_, wr);

        records::Acceptance a{};
        a.work        = ref_to(alice_, work);
        a.hours_raw   = hours;
        a.labor_units = units;
        for (const auto& [tag, u] : lines) a.axes.push_back({test_axis(tag), u});

        records::Transfer t{};
        t.from      = payer.bytes;
        t.to        = alice_.bytes;
        t.origins   = { {payer.bytes, units} };
        t.accept    = a;
        t.timestamp = kDay + 100;
        return ref_to(payer, add(payer, t));
    }
};

TEST_F(RatesViewTest, SettledDealsAverageSmoothAndCarryForward) {
    // The bread example from main_ideas.pdf: (0.3+0.5)/(0.2+0.4) = 1.3333.
    settled_deal(bob_, 0.2, 0.3);
    settled_deal(bob_, 0.4, 0.5);

    // An unsettled declaration («искажающая пропорция» without payment) cannot
    // even be written any more: the acceptance rides inside its payment, and the
    // codec refuses one that does not pay exactly (records.md §9.5).

    // No previous prices → pure day average.
    const auto fresh = build_daily_prices(*storage_, kDay, {});
    ASSERT_EQ(fresh.axes.size(), 1u);
    EXPECT_EQ(fresh.axes[0].axis, test_axis());
    EXPECT_NEAR(fresh.axes[0].price, 0.8 / 0.6, 1e-9);
    EXPECT_EQ(fresh.axes[0].deals, 2u);
    EXPECT_NEAR(fresh.W, 0.8 / 0.6, 1e-9);

    // With yesterday's 1.25: price = 0.3*1.3333 + 0.7*1.25 = 1.275, and an
    // untouched axis carries forward unchanged.
    const std::vector<records::AxisPrice> prev = {
        {test_axis(),     1.25, 0.0, 0, 0.0},
        {test_axis(0x0F), 14.0, 0.0, 0, 0.0},
    };
    const auto smoothed = build_daily_prices(*storage_, kDay, prev);
    ASSERT_EQ(smoothed.axes.size(), 2u);
    EXPECT_NEAR(price_of(smoothed.axes, 0x01)->price, 0.3 * (0.8 / 0.6) + 0.7 * 1.25, 1e-9);
    EXPECT_DOUBLE_EQ(price_of(smoothed.axes, 0x0F)->price, 14.0);
    EXPECT_EQ(price_of(smoothed.axes, 0x0F)->deals, 0u);
}

TEST_F(RatesViewTest, SelfDealsAndTinyVolumesExcluded) {
    // A self-deal cannot even be written: nobody accepts their own work.
    EXPECT_THROW(settled_deal(alice_, 0.5, 20.0), records::CodecError);
    EXPECT_TRUE(build_daily_prices(*storage_, kDay, {}).axes.empty());

    // Below min_hours, no previous price: the axis gets none. There is no prior
    // to borrow one from — nobody has paid for it yet.
    settled_deal(bob_, 0.05, 5.0);
    EXPECT_TRUE(build_daily_prices(*storage_, kDay, {}).axes.empty());
    // ...but with a previous price the axis inherits it unchanged.
    const std::vector<records::AxisPrice> prev = {{test_axis(), 1.25, 0.0, 0, 0.0}};
    const auto inherited = build_daily_prices(*storage_, kDay, prev);
    ASSERT_EQ(inherited.axes.size(), 1u);
    EXPECT_DOUBLE_EQ(inherited.axes[0].price, 1.25);
}

TEST_F(RatesViewTest, CarriedCostSettlesAtFullPayableButAveragesLaborOnly) {
    // Хлеб с печью (ИР-011, records.md §9.5 v2): labor 6.0 за 6ч плюс
    // перенос печи 0.23625ч. Потолок оплаты = 6.23625.
    records::WorkRecord wr{};
    wr.hours = 6.0;
    records::CarryEntry ce{};
    ce.src.chain = alice_.bytes;
    ce.src.hash.fill(0x61);
    ce.used    = 6.0;
    ce.carried = 0.23625;
    ce.seq     = 0;
    ce.after   = 0.23625;
    wr.carry.push_back(ce);
    wr.customer = bob_.bytes;   // заказ Боба (records.md §9.4)
    const Block work = add(alice_, wr);

    records::Acceptance a{};
    a.work          = ref_to(alice_, work);
    a.hours_raw     = 6.0;
    a.labor_units   = 6.0;
    a.axes          = {{test_axis(), 6.0}};
    a.carried_units = 0.23625;

    // Paying the labor part alone is not a deal («=» records.md §12.8 is labor +
    // carried): the codec refuses to write it.
    records::Transfer t{};
    t.from      = bob_.bytes;
    t.to        = alice_.bytes;
    t.origins   = { {bob_.bytes, 6.0} };
    t.accept    = a;
    t.timestamp = kDay + 100;
    EXPECT_THROW(records::Codec::encode(t), records::CodecError);

    // Paid in full, it is. The price is labor/hours = 1.0: carried cost must NOT
    // distort the price of live labor (records.md §11.2).
    t.origins = { {bob_.bytes, 6.23625} };
    add(bob_, t);
    const auto d = build_daily_prices(*storage_, kDay, {});
    ASSERT_EQ(d.axes.size(), 1u);
    EXPECT_NEAR(d.axes[0].price, 1.0, 1e-9);
    EXPECT_EQ(d.axes[0].deals, 1u);
}

// W — средняя бумага на трудочас сети (economy.md §2б), хотя часы сделки входят
// во все оси, которые она назвала: Σ цена·часы по осям сходится с тем, что
// сделки заплатили, а делится на часы, которые они на самом деле заняли.
TEST_F(RatesViewTest, WIsTheAveragePaperPerLabourHour) {
    settled_deal(bob_, 2.0, 2.0, {{0x01, 1.0}, {0x02, 1.0}});   // 1.0 за час, две оси
    settled_deal(bob_, 1.0, 3.0, {{0x01, 3.0}});                // 3.0 за час, одна
    const auto d = build_daily_prices(*storage_, kDay, {});
    ASSERT_EQ(d.axes.size(), 2u);
    EXPECT_NEAR(price_of(d.axes, 0x01)->price, 4.0 / 3.0, 1e-12);   // 4 стч за 3 часа
    EXPECT_NEAR(price_of(d.axes, 0x02)->price, 0.5, 1e-12);         // 1 стч за 2 часа
    EXPECT_EQ(price_of(d.axes, 0x01)->deals, 2u);
    EXPECT_NEAR(d.W, 5.0 / 3.0, 1e-12);                             // 5 стч за 3 часа
}

TEST_F(RatesViewTest, RatesEndpointPublishesSignedAggregateOnce) {
    settled_deal(bob_, 0.2, 0.3);

    const uint16_t port = 18790;
    AggregatorServer server(*storage_, port, {}, std::chrono::seconds(3600),
                            std::chrono::seconds(3600), db_path_ / "own");
    std::thread th([&] { server.run(); });
    httplib::Client cli("127.0.0.1:" + std::to_string(port));
    for (int i = 0; i < 100; ++i) {
        if (auto r = cli.Get("/stats"); r && r->status == 200) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    auto r1 = cli.Get("/economy/rates");
    auto r2 = cli.Get("/economy/rates");

    ASSERT_TRUE(r1 && r1->status == 200) << (r1 ? r1->body : "no response");
    ASSERT_TRUE(r2 && r2->status == 200);
    EXPECT_EQ(r1->body, r2->body);   // published once, then served from chain
    EXPECT_NE(r1->body.find("\"axes\":["), std::string::npos);
    ASSERT_NE(r1->body.find("\"block\":\""), std::string::npos);
    // The published block entered the warehouse: fetchable for verification.
    const auto pos = r1->body.find("\"block\":\"");
    const std::string bh = r1->body.substr(pos + 9, 64);
    auto blk = cli.Get("/sync/block/" + bh);

    server.stop();
    th.join();
    ASSERT_TRUE(blk && blk->status == 200);
}

// Разбивка ЕСТЬ цена (records.md §9.5 v4): не сходящаяся с ценой описывает
// какую-то другую сделку — в цены осей она не входит, как не входит в книгу осей.
TEST_F(RatesViewTest, BreakdownThatDoesNotAddUpIsNotAPrice) {
    records::WorkRecord wr{};
    wr.hours = 2.0;
    wr.customer = bob_.bytes;   // заказ Боба (records.md §9.4)
    const Block work = add(alice_, wr);
    records::Acceptance a{};
    a.work        = ref_to(alice_, work);
    a.hours_raw   = 2.0;
    a.labor_units = 8.0;
    a.axes        = {{test_axis(), 2.0}};            // названо 2 из 8
    records::Transfer t{};
    t.from      = bob_.bytes;
    t.to        = alice_.bytes;
    t.origins   = { {bob_.bytes, 8.0} };
    t.accept    = a;
    t.timestamp = kDay + 100;
    add(bob_, t);
    EXPECT_TRUE(build_daily_prices(*storage_, kDay, {}).axes.empty());
}

// ── ИР-021: вес сделки по независимости контрагентов ─────────────────────────
//
// Проверяем именно КОНЪЮНКЦИЮ: дисконт обязан требовать обе половины сразу.
// Честная деревня взаимна, но торгует по обычной цене; одинокая премия дорога,
// но не взаимна. Наказывать нужно только совпадение того и другого.
class IndependenceTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;

    UserId alice_ = make_chain(0xA1);
    UserId carol_ = make_chain(0xC3);
    UserId dave_  = make_chain(0xD4);

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_indep_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    Block add(const UserId& owner, const records::Record& rec) {
        const Block b = make_record_block(owner, next_index_++, rec);
        EXPECT_TRUE(storage_->add_block(b));
        return b;
    }

    // Сделка с разбивкой по осям: {тег оси, стч за час}. Работник `worker`
    // отработал `hours`, плательщик `payer` принял и заплатил ровно оценку.
    void deal_on(const UserId& payer, const UserId& worker, double hours,
                 const std::vector<std::pair<uint8_t, double>>& per_hour, int64_t ts) {
        records::WorkRecord wr{};
        wr.hours = hours;
        wr.customer = payer.bytes;   // на заказ плательщика (records.md §9.4)
        const Block work = add(worker, wr);

        double units = 0.0;
        records::Acceptance a{};
        for (const auto& [tag, r] : per_hour) {
            a.axes.push_back({test_axis(tag), r * hours});
            units += r * hours;
        }
        a.work        = ref_to(worker, work);
        a.hours_raw   = hours;
        a.labor_units = units;

        records::Transfer t{};
        t.from      = payer.bytes;
        t.to        = worker.bytes;
        t.origins   = { {payer.bytes, units} };
        t.accept    = a;
        t.timestamp = ts;
        add(payer, t);
    }

    // Рассчитанная сделка на общей оси: worker отработал `hours` по цене `rate`.
    void deal(const UserId& worker, const UserId& payer,
              double hours, double rate, int64_t ts) {
        deal_on(payer, worker, hours, {{0x01, rate}}, ts);
    }

    // Фон сети: десять односторонних сделок разных пар по обычной цене 1.0 на
    // общей оси. Задают её сетевую цену, относительно которой меряется аномалия.
    void honest_background(int64_t ts, uint8_t n = 10) {
        for (uint8_t i = 0; i < n; ++i)
            deal(make_chain(static_cast<uint8_t>(0x10 + i)),
                 make_chain(static_cast<uint8_t>(0x30 + i)), 1.0, 1.0, ts);
    }

    DailyPrices plain() { return build_daily_prices(*storage_, kDay, {}); }
    DailyPrices weighted(const IndependenceParams& p = {},
                         const std::vector<records::AxisPrice>& prev = {}) {
        return build_daily_prices(*storage_, kDay, prev, 0.3, 0.1, &p);
    }
};

TEST_F(IndependenceTest, HonestReciprocalVillageKeepsFullWeight) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);
    // Двое соседей весь месяц работают друг на друга по ОБЫЧНОЙ цене.
    for (int i = 0; i < 4; ++i) {
        deal(alice_, carol_, 1.0, 1.0, before);
        deal(carol_, alice_, 1.0, 1.0, before);
    }
    deal(alice_, carol_, 1.0, 1.0, kDay + 100);          // сделка дня

    const auto p = plain();
    const auto w = weighted();
    ASSERT_EQ(p.axes.size(), 1u);
    ASSERT_EQ(w.axes.size(), 1u);
    // Взаимность есть, аномалии нет → вес полный, цена не шелохнулась.
    EXPECT_NEAR(w.axes[0].price, p.axes[0].price, 1e-9);
    EXPECT_NEAR(w.axes[0].price, 1.0, 1e-9);
    EXPECT_NEAR(w.axes[0].hours, p.axes[0].hours, 1e-9);
}

TEST_F(IndependenceTest, CollusiveReciprocalPairIsDiscounted) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);
    // Пара накручивает k втрое, гоняя бумагу туда-обратно: работа может быть
    // настоящей, слоты не пересекаются — накручена ОЦЕНКА.
    for (int i = 0; i < 4; ++i) {
        deal(alice_, carol_, 1.0, 3.0, before);
        deal(carol_, alice_, 1.0, 3.0, before);
    }
    deal(alice_, carol_, 1.0, 3.0, kDay + 100);          // сговорная сделка дня
    deal(make_chain(0x41), make_chain(0x51), 1.0, 1.0, kDay + 100);  // честная

    const auto p = plain();
    const auto w = weighted();
    ASSERT_EQ(p.axes.size(), 1u);
    ASSERT_EQ(w.axes.size(), 1u);
    EXPECT_NEAR(p.axes[0].price, 2.0, 1e-9);     // (3.0 + 1.0) / 2 — сговор прошёл
    // Дисконт МЯГКИЙ (развилка B3), а не отсечение: потоки пары 15 и 12, по
    // циклу длины 2 вернулось 12, значит R = 12/15 = 0.8; аномалия втрое выше
    // цены оси → вес 0.2. Среднее дня = (0.2·3 + 1·1) / (0.2 + 1) = 1.333 вместо
    // 2.0: две трети накрутки снято.
    EXPECT_NEAR(w.axes[0].price, 4.0 / 3.0, 1e-3);
    // Часы остаются сырыми: труд-то был, под вопросом его оценка.
    EXPECT_NEAR(w.axes[0].hours, 2.0, 1e-9);
    EXPECT_EQ(w.axes[0].deals, 2u);
}

TEST_F(IndependenceTest, OneSidedPremiumIsNotDiscounted) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);
    // Дорогая, но ОДНОСТОРОННЯЯ сделка: заказчик у работника ничего не покупал
    // в ответ. Это законная премия — трогать её нельзя.
    deal(alice_, carol_, 1.0, 3.0, kDay + 100);

    const auto p = plain();
    const auto w = weighted();
    ASSERT_EQ(w.axes.size(), 1u);
    EXPECT_NEAR(w.axes[0].price, p.axes[0].price, 1e-9);
    EXPECT_NEAR(w.axes[0].price, 3.0, 1e-9);
}

TEST_F(IndependenceTest, ShortWindowMissesAlternatingCollusion) {
    // Почему окно обязано быть длинным (B2): пара, чередующая роли по месяцам,
    // внутри короткого окна выглядит односторонней и ускользает целиком.
    const int64_t month = 86'400 * 30;
    honest_background(kDay - month * 2);
    for (int i = 0; i < 4; ++i) {
        deal(alice_, carol_, 1.0, 3.0, kDay - month * 2);   // позапрошлый месяц
        deal(carol_, alice_, 1.0, 3.0, kDay - month);       // прошлый: роли сменились
    }
    deal(alice_, carol_, 1.0, 3.0, kDay + 100);

    IndependenceParams short_win{};
    short_win.window_days = 20;                 // короче периода чередования
    IndependenceParams long_win{};              // 365 дней по умолчанию

    // Вчерашняя цена оси — 1.0: за день торговала одна лишь пара, так что разница
    // видна в том, насколько ей позволено сдвинуть цену.
    const std::vector<records::AxisPrice> prev = {{test_axis(), 1.0, 0.0, 0, 0.0}};
    const auto s = weighted(short_win, prev);
    const auto l = weighted(long_win, prev);
    ASSERT_EQ(s.axes.size(), 1u);
    ASSERT_EQ(l.axes.size(), 1u);
    // Короткое окно слепо: пара выглядит односторонней, доверие ко дню полное →
    // обычное сглаживание 0.3·3.0 + 0.7·1.0 = 1.6.
    EXPECT_NEAR(s.axes[0].price, 1.6, 1e-9);
    // Длинное окно видит взаимность: R = 12/15 = 0.8 → вес 0.2, доверие ко дню
    // 0.2, сглаживание 0.3·0.2 = 0.06, и вчерашняя цена почти не двигается:
    // 0.06·3.0 + 0.94·1.0 = 1.12.
    EXPECT_NEAR(l.axes[0].price, 1.12, 1e-9);
}

TEST_F(IndependenceTest, ThreeChainRingIsCaughtOnlyByCycleSearch) {
    // B5: кольцо A→B→C→A невидимо любой парной мере — между каждой парой поток
    // строго односторонний, — а вредит ровно так же. Ловит только поиск циклов.
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before, 20);
    for (int i = 0; i < 4; ++i) {
        deal(alice_, carol_, 1.0, 3.0, before);   // carol платит alice
        deal(carol_, dave_,  1.0, 3.0, before);   // dave платит carol
        deal(dave_,  alice_, 1.0, 3.0, before);   // alice платит dave
    }
    deal(alice_, carol_, 1.0, 3.0, kDay + 100);                     // кольцевая
    deal(make_chain(0x71), make_chain(0x81), 1.0, 1.0, kDay + 100); // честная

    IndependenceParams pairs_only{};
    pairs_only.max_cycle = 2;                  // только пары, как было до B5
    IndependenceParams rings{};                // max_cycle = 4 по умолчанию

    const auto p = weighted(pairs_only);
    const auto r = weighted(rings);
    ASSERT_EQ(p.axes.size(), 1u);
    ASSERT_EQ(r.axes.size(), 1u);
    // Парная мера слепа: двухзвенных циклов в кольце нет, вес полный.
    EXPECT_NEAR(p.axes[0].price, 2.0, 1e-9);
    // Поиск циклов находит треугольник: потоки 15/12/12, узкое место 12,
    // R = 12/15 = 0.8 → вес 0.2, среднее дня (0.2·3 + 1·1)/(0.2 + 1) = 1.333.
    EXPECT_NEAR(r.axes[0].price, 4.0 / 3.0, 1e-3);
}

// Опора — среднее арифметическое по сделкам (ратифицировано 2026-09-08): сделки
// не схлопываются по рёбрам. Если большинство честно, то каждая сделка есть
// настоящий труд и обязана считаться, даже когда двое работают только друг с
// другом.
TEST_F(IndependenceTest, DealMeanReferenceCatchesModerateFlooding) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);                       // 10 сделок по цене 1.0
    for (int i = 0; i < 3; ++i) {                    // 6 сделок пары по 3.0
        deal(alice_, carol_, 1.0, 3.0, before);
        deal(carol_, alice_, 1.0, 3.0, before);
    }
    deal(alice_, carol_, 1.0, 3.0, kDay + 100);                     // сговорная
    deal(make_chain(0x71), make_chain(0x81), 1.0, 1.0, kDay + 100); // честная

    const auto w = weighted();
    ASSERT_EQ(w.axes.size(), 1u);
    // Сговорная сделка втрое дороже опоры и полностью кругооборотна — её вес
    // падает, и среднее дня тянется к честной цене, а не к 2.0.
    EXPECT_LT(w.axes[0].price, 1.5);
    EXPECT_NEAR(w.axes[0].hours, 2.0, 1e-9);         // часы сырые: труд был
    EXPECT_LT(w.axes[0].weighted_hours, w.axes[0].hours);
}

// ЧЕМ ЗА ЭТО ПЛАТИМ, честно и в тесте. Опора по рёбрам была неуязвима к числу
// сделок: пара давала одну точку, сколько бы она ни наторговала. Опора по
// сделкам этим свойством не обладает — залив ось, пара утягивает опору к себе и
// перестаёт выглядеть аномальной.
//
// Замерено на прогоне года (цена ×1.3): при опоре по рёбрам аномалия остаётся
// 1.28x при любой доле сделок; при среднем по сделкам она сползает
// 1.28 → 1.21 (28% сделок) → 1.16 (43%) → 1.11 (61%) → 1.07 (75%).
//
// Это принятая цена, а не недосмотр: заливать значит заявлять ЧАСЫ, а часы
// дороги (economy.md §3) — их подделка требует свидетелей и непересекающихся
// слотов. Кругооборотная половина конъюнкции при этом продолжает работать:
// заливающая пара взаимна по построению.
TEST_F(IndependenceTest, FloodingTheAxisBluntsTheAnomalyHalf) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);                       // 10 честных сделок по 1.0
    for (int i = 0; i < 30; ++i) {                   // 60 сделок пары: 86% оси
        deal(alice_, carol_, 1.0, 1.3, before);
        deal(carol_, alice_, 1.0, 1.3, before);
    }
    deal(alice_, carol_, 1.0, 1.3, kDay + 100);
    deal(make_chain(0x71), make_chain(0x81), 1.0, 1.0, kDay + 100);

    const auto w = weighted();
    ASSERT_EQ(w.axes.size(), 1u);
    // Опора уехала к цене пары: аномалия сжалась с 1.30 до ≈1.03. Дисконт не
    // исчезает совсем — его удерживает нижний зажим kappa_min = 1.10, — но
    // слабеет резко: уцелевает ≈82% объёма против ≈8% у той же пары на
    // незалитой оси (CollusiveReciprocalPairIsDiscounted).
    EXPECT_GT(w.axes[0].weighted_hours / w.axes[0].hours, 0.75);
    EXPECT_LT(w.axes[0].weighted_hours, w.axes[0].hours);   // но и не ноль
}

// Цена оси появляется, когда её назвали хотя бы `min_axis_payers` разных
// плательщиков: ось стоит столько, сколько за неё платят независимые люди
// (ИР-022). Сделка, ни одна ось которой ещё не оценена, не судится вовсе:
// наказать честную молодую работу хуже, чем пропустить.
TEST_F(IndependenceTest, UnpricedAxesAreNotJudged) {
    const int64_t before = kDay - 86'400 * 30;
    deal(alice_, carol_, 1.0, 1.0, before);          // ось назвали двое плательщиков
    deal(carol_, alice_, 1.0, 1.0, before);
    deal(alice_, carol_, 1.0, 9.0, kDay + 100);      // вопиюще дорогая и взаимная

    const auto w = weighted();                       // min_axis_payers = 4
    ASSERT_EQ(w.axes.size(), 1u);
    EXPECT_NEAR(w.axes[0].price, 9.0, 1e-9);         // не судили вовсе
    EXPECT_NEAR(w.axes[0].weighted_hours, w.axes[0].hours, 1e-9);
}

// Как только ось назвали четверо разных плательщиков — у неё есть цена, и та же
// сделка уценивается. Порог считает ПЛАТЕЛЬЩИКОВ, а не сделки: пара сколько
// угодно раз назовёт свою ось, но оценить её сама не сможет.
TEST_F(IndependenceTest, FourPayersPriceTheAxis) {
    const int64_t before = kDay - 86'400 * 30;
    deal(alice_, carol_, 1.0, 1.0, before);
    deal(carol_, alice_, 1.0, 1.0, before);
    deal(make_chain(0x71), make_chain(0x81), 1.0, 1.0, before);   // третий
    deal(make_chain(0x72), make_chain(0x82), 1.0, 1.0, before);   // четвёртый
    deal(alice_, carol_, 1.0, 9.0, kDay + 100);

    const auto w = weighted();
    ASSERT_EQ(w.axes.size(), 1u);
    EXPECT_LT(w.axes[0].weighted_hours, w.axes[0].hours);
}

// Вариант «б» (ИР-022, выбран 2026-09-11): опора — сетевая цена того, что сделка
// НАЗВАЛА. Надбавка мастера, названная своей осью, оценена всеми, кто эту ось
// называет, и аномалией не является — даже в деревне, где деньги ходят по кругу.
TEST_F(IndependenceTest, NamedPremiumIsPricedByTheNetwork) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);                                // ось A: 1.0 за час
    for (uint8_t i = 0; i < 4; ++i)                           // четверо платят мастерам
        deal_on(make_chain(static_cast<uint8_t>(0x50 + i)),
                make_chain(static_cast<uint8_t>(0x60 + i)), 1.0,
                {{0x01, 1.0}, {0x02, 0.3}}, before);          // A + мастерство 0.3
    for (int i = 0; i < 3; ++i) {                             // деревня: круговорот
        deal_on(alice_, carol_, 1.0, {{0x01, 1.0}, {0x02, 0.3}}, before);
        deal_on(carol_, alice_, 1.0, {{0x01, 1.0}, {0x02, 0.3}}, before);
    }
    deal_on(alice_, carol_, 1.0, {{0x01, 1.0}, {0x02, 0.3}}, kDay + 100);

    const auto w = weighted();
    ASSERT_EQ(w.axes.size(), 2u);
    for (const auto& p : w.axes)
        EXPECT_NEAR(p.weighted_hours, p.hours, 1e-9);         // не уценена
}

// Та же надбавка, спрятанная в общую ось, — аномалия: объяснить её нечем.
TEST_F(IndependenceTest, PremiumHiddenInACommonAxisIsAnAnomaly) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);
    for (uint8_t i = 0; i < 4; ++i)
        deal_on(make_chain(static_cast<uint8_t>(0x50 + i)),
                make_chain(static_cast<uint8_t>(0x60 + i)), 1.0,
                {{0x01, 1.0}, {0x02, 0.3}}, before);
    for (int i = 0; i < 3; ++i) {
        deal_on(alice_, carol_, 1.0, {{0x01, 1.3}}, before);
        deal_on(carol_, alice_, 1.0, {{0x01, 1.3}}, before);
    }
    deal_on(alice_, carol_, 1.0, {{0x01, 1.3}}, kDay + 100);

    const auto w = weighted();
    ASSERT_EQ(w.axes.size(), 1u);
    EXPECT_LT(w.axes[0].weighted_hours / w.axes[0].hours, 0.5);
}

// Своя ось не оценивает сама себя. Пара, заведшая ось только для себя, не может
// спрятать в неё завышение: пока ось назвали меньше четырёх разных плательщиков,
// оплата на ней не объяснена, и там, где поток ходит по кругу, она уценивается.
TEST_F(IndependenceTest, PrivateAxisDoesNotPriceItsOwnInflation) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);
    for (int i = 0; i < 3; ++i) {
        deal_on(alice_, carol_, 1.0, {{0x01, 1.0}, {0x03, 0.3}}, before);
        deal_on(carol_, alice_, 1.0, {{0x01, 1.0}, {0x03, 0.3}}, before);
    }
    deal_on(alice_, carol_, 1.0, {{0x01, 1.0}, {0x03, 0.3}}, kDay + 100);

    const auto w  = weighted();
    const auto* a = price_of(w.axes, 0x01);
    ASSERT_NE(a, nullptr);
    EXPECT_LT(a->weighted_hours / a->hours, 0.5);
}

// Засчитывается сделка того, кому продавец отдал результат (records.md §9.5):
// посторонний, заплативший за чужую работу, цены не двигает — его сделка
// ничтожна; работа, которую продавец сам отдал двоим, — двойная продажа, и в
// цены осей не входит ни одна из её сделок, как бы дорого за неё ни заплатили.
TEST_F(RatesViewTest, OnlyTheAddressedDealMovesThePrice) {
    const UserId carol = make_chain(0xC7);
    settled_deal(bob_, 1.0, 1.0);                 // честная сделка дня

    const auto pay = [&](const Block& work, const UserId& buyer,
                         std::optional<records::Ref> release) {
        records::Acceptance a{};
        a.work        = ref_to(alice_, work);
        a.hours_raw   = 1.0;
        a.labor_units = 9.0;
        a.axes        = {{test_axis(), 9.0}};
        a.release     = release;
        records::Transfer t{};
        t.from      = buyer.bytes;
        t.to        = alice_.bytes;
        t.origins   = { {buyer.bytes, 9.0} };
        t.accept    = a;
        t.timestamp = kDay + 100;
        add(buyer, t);
    };

    records::WorkRecord wr{};
    wr.hours    = 1.0;
    wr.customer = bob_.bytes;                     // заказ Боба
    const Block ordered = add(alice_, wr);
    pay(ordered, carol, std::nullopt);            // Кэрол платит без адреса — ничтожна

    wr.action = "вторая работа";
    const Block sold_twice = add(alice_, wr);
    records::Release rl{};                        // Алиса сама отдаёт её и Кэрол
    rl.work      = ref_to(alice_, sold_twice);
    rl.to        = carol.bytes;
    rl.timestamp = kDay;
    rl.expires   = kDay + 1000;
    const Block released = add(alice_, rl);
    pay(sold_twice, bob_, std::nullopt);          // заказчик
    pay(sold_twice, carol, ref_to(alice_, released));   // и по отпуску — двойная продажа

    const auto day = build_daily_prices(*storage_, kDay, {});
    ASSERT_EQ(day.axes.size(), 1u);
    EXPECT_EQ(day.axes[0].deals, 1u);             // только честная
    EXPECT_NEAR(day.axes[0].price, 1.0, 1e-9);
}
