#include "aggregator/rates_view.h"
#include "aggregator/cloud_view.h"
#include "aggregator/server.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>
#include <httplib.h>

#include <filesystem>
#include <thread>

// Ось для разбивки цены: в этих тестах её содержание не проверяется, важно лишь
// что приёмки без разбивки не существует (records.md §9.5 v4).
inline records::BlockPath test_axis() {
    records::BlockPath r{};
    r.chain.fill(0x79);
    r.hash.fill(0x01);
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

} // namespace

class RatesViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    UserId alice_ = make_chain(0xA1);   // worker
    UserId bob_   = make_chain(0xB2);   // payer

    records::Ref grade_ref_{};

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_rates_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);

        const Block spec = add(alice_, records::Specialty{"хлебопёк"});
        records::Grade g{};
        g.specialty = ref_to(alice_, spec);
        g.level     = 3;
        grade_ref_  = ref_to(alice_, add(alice_, g));
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

    // One settled deal: work → acceptance (by `payer`) → settlement transfer.
    // Returns the Acceptance ref.
    records::Ref settled_deal(const UserId& payer, double hours, double units) {
        records::WorkRecord wr{};
        wr.agent = grade_ref_;
        wr.hours = hours;
        const Block work = add(alice_, wr);

        records::Acceptance a{};
        a.work        = ref_to(alice_, work);
        a.receiver    = payer.bytes;
        a.hours_raw   = hours;
        a.labor_units = units;
        a.axes        = {{test_axis(), units}};
        a.timestamp   = kDay + 100;
        const Block acc = add(payer, a);

        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = alice_.bytes;
        t.origins = { {payer.bytes, units} };
        t.reason  = ref_to(payer, acc);
        add(payer, t);
        return ref_to(payer, acc);
    }
};

TEST_F(RatesViewTest, SettledDealsAverageSmoothAndCarryForward) {
    // The bread example from main_ideas.pdf: (0.3+0.5)/(0.2+0.4) = 1.3333.
    settled_deal(bob_, 0.2, 0.3);
    settled_deal(bob_, 0.4, 0.5);

    // Unsettled declaration (no payment): must not move the rate.
    records::WorkRecord wr{};
    wr.agent = grade_ref_;
    wr.hours = 1.0;
    const Block work = add(alice_, wr);
    records::Acceptance fake{};
    fake.work        = ref_to(alice_, work);
    fake.receiver    = bob_.bytes;
    fake.hours_raw   = 1.0;
    fake.labor_units = 40.0;          // "искажающая пропорция"
    fake.axes        = {{test_axis(), 40.0}};
    fake.timestamp   = kDay + 100;
    add(bob_, fake);

    // No previous rates → pure day average.
    const auto fresh = build_daily_rates(*storage_, kDay, {});
    ASSERT_EQ(fresh.size(), 1u);
    EXPECT_EQ(fresh[0].specialty, "хлебопёк");
    EXPECT_NEAR(fresh[0].rate, 0.8 / 0.6, 1e-9);
    EXPECT_EQ(fresh[0].deals, 2u);

    // With yesterday's 1.25: rate = 0.3*1.3333 + 0.7*1.25 = 1.275, and an
    // untouched specialty carries forward unchanged.
    const std::vector<records::RateEntry> prev = {
        {"хлебопёк", 1.25, 0.0, 0},
        {"кардиохирург", 14.0, 0.0, 0},
    };
    const auto smoothed = build_daily_rates(*storage_, kDay, prev);
    ASSERT_EQ(smoothed.size(), 2u);
    EXPECT_EQ(smoothed[0].specialty, "кардиохирург");
    EXPECT_DOUBLE_EQ(smoothed[0].rate, 14.0);
    EXPECT_EQ(smoothed[0].deals, 0u);
    EXPECT_NEAR(smoothed[1].rate, 0.3 * (0.8 / 0.6) + 0.7 * 1.25, 1e-9);
}

TEST_F(RatesViewTest, SelfDealsAndTinyVolumesExcluded) {
    settled_deal(alice_, 0.5, 20.0);          // self-deal: payer == worker
    const auto rates = build_daily_rates(*storage_, kDay, {});
    EXPECT_TRUE(rates.empty());

    settled_deal(bob_, 0.05, 5.0);            // below min_hours, no previous
    EXPECT_TRUE(build_daily_rates(*storage_, kDay, {}).empty());
    // ...but with a previous rate the specialty inherits it unchanged.
    const std::vector<records::RateEntry> prev = {{"хлебопёк", 1.25, 0, 0}};
    const auto inherited = build_daily_rates(*storage_, kDay, prev);
    ASSERT_EQ(inherited.size(), 1u);
    EXPECT_DOUBLE_EQ(inherited[0].rate, 1.25);
}

TEST_F(RatesViewTest, CarriedCostSettlesAtFullPayableButAveragesLaborOnly) {
    // Хлеб с печью (ИР-011, records.md §9.5 v2): labor 6.0 за 6ч плюс
    // перенос печи 0.23625ч. Потолок оплаты = 6.23625.
    records::WorkRecord wr{};
    wr.agent = grade_ref_;
    wr.hours = 6.0;
    records::CarryEntry ce{};
    ce.src.chain = alice_.bytes;
    ce.src.hash.fill(0x61);
    ce.used    = 6.0;
    ce.carried = 0.23625;
    ce.seq     = 0;
    ce.after   = 0.23625;
    wr.carry.push_back(ce);
    const Block work = add(alice_, wr);

    records::Acceptance a{};
    a.work          = ref_to(alice_, work);
    a.receiver      = bob_.bytes;
    a.hours_raw     = 6.0;
    a.labor_units   = 6.0;
    a.axes          = {{test_axis(), 6.0}};
    a.carried_units = 0.23625;
    a.timestamp     = kDay + 100;
    const Block acc = add(bob_, a);

    // Paying the labor part alone leaves the deal unsettled («=» records.md §12.8 is
    // now labor + carried) — it must not move the rates.
    records::Transfer t{};
    t.from    = bob_.bytes;
    t.to      = alice_.bytes;
    t.origins = { {bob_.bytes, 6.0} };
    t.reason  = ref_to(bob_, acc);
    add(bob_, t);
    EXPECT_TRUE(build_daily_rates(*storage_, kDay, {}).empty());

    // Topping up the carried part settles it. The rate is labor/hours = 1.0:
    // carried cost must NOT distort the price of live labor (records.md §11.2 v2).
    records::Transfer t2 = t;
    t2.origins = { {bob_.bytes, 0.23625} };
    add(bob_, t2);
    const auto rates = build_daily_rates(*storage_, kDay, {});
    ASSERT_EQ(rates.size(), 1u);
    EXPECT_NEAR(rates[0].rate, 1.0, 1e-9);
    EXPECT_EQ(rates[0].deals, 1u);
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
    ASSERT_NE(r1->body.find("\"block\":\""), std::string::npos);
    // The published block entered the warehouse: fetchable for verification.
    const auto pos = r1->body.find("\"block\":\"");
    const std::string bh = r1->body.substr(pos + 9, 64);
    auto blk = cli.Get("/sync/block/" + bh);

    server.stop();
    th.join();
    ASSERT_TRUE(blk && blk->status == 200);
}

// Cloud prior (ИР-018, specialty-axes.md §10): a thin, previously-unseen bucket is
// seeded from its cloud neighbour's rate instead of being skipped.
TEST_F(RatesViewTest, CloudPriorSeedsThinNewSpecialty) {
    settled_deal(bob_, 0.05, 5.0);            // "хлебопёк", thin, no previous

    records::SpecialtyCloud cloud{};
    records::CloudPoint p{};
    p.slug = "хлебопёк";
    p.neighbors.push_back({"prof.cook", 1.0});
    cloud.points.push_back(p);

    const std::vector<records::RateEntry> prev = {{"prof.cook", 1.2, 0.0, 0}};

    const auto rates = build_daily_rates(*storage_, kDay, prev, 0.3, 0.1, &cloud);
    bool seeded = false;
    for (const auto& r : rates)
        if (r.specialty == "хлебопёк") {
            EXPECT_DOUBLE_EQ(r.rate, 1.2);    // neighbour's rate
            seeded = true;
        }
    EXPECT_TRUE(seeded);

    // Without a cloud the thin bucket stays skipped (unchanged behaviour).
    bool present = false;
    for (const auto& r : build_daily_rates(*storage_, kDay, prev))
        if (r.specialty == "хлебопёк") present = true;
    EXPECT_FALSE(present);
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
    records::Ref                       grade_ref_{};

    UserId alice_ = make_chain(0xA1);
    UserId carol_ = make_chain(0xC3);
    UserId dave_  = make_chain(0xD4);

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_indep_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);

        // Одна специальность и один разряд на всех: все сделки попадают в одну
        // корзину, значит у аномалии есть общая опора-медиана.
        const Block spec = add(alice_, records::Specialty{"хлебопёк"});
        records::Grade g{};
        g.specialty = ref_to(alice_, spec);
        g.level     = 3;
        grade_ref_  = ref_to(alice_, add(alice_, g));
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

    // Рассчитанная сделка: worker отработал `hours` по цене `rate` за час,
    // payer принял и заплатил ровно оценку.
    void deal(const UserId& worker, const UserId& payer,
              double hours, double rate, int64_t ts) {
        records::WorkRecord wr{};
        wr.agent = grade_ref_;
        wr.hours = hours;
        const Block work = add(worker, wr);

        records::Acceptance a{};
        a.work        = ref_to(worker, work);
        a.receiver    = payer.bytes;
        a.hours_raw   = hours;
        a.labor_units = hours * rate;
        a.axes        = {{test_axis(), hours * rate}};
        a.timestamp   = ts;
        const Block acc = add(payer, a);

        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = worker.bytes;
        t.origins = { {payer.bytes, hours * rate} };
        t.reason  = ref_to(payer, acc);
        add(payer, t);
    }

    // Фон корзины: десять односторонних сделок разных пар по обычной цене 1.0.
    // Задают медиану, относительно которой меряется аномалия.
    void honest_background(int64_t ts, uint8_t n = 10) {
        for (uint8_t i = 0; i < n; ++i)
            deal(make_chain(static_cast<uint8_t>(0x10 + i)),
                 make_chain(static_cast<uint8_t>(0x30 + i)), 1.0, 1.0, ts);
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

    const IndependenceParams indep{};
    const auto plain = build_daily_rates(*storage_, kDay, {});
    const auto weighted =
        build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr, &indep);

    ASSERT_EQ(plain.size(), 1u);
    ASSERT_EQ(weighted.size(), 1u);
    // Взаимность есть, аномалии нет → вес полный, ставка не шелохнулась.
    EXPECT_NEAR(weighted[0].rate, plain[0].rate, 1e-9);
    EXPECT_NEAR(weighted[0].rate, 1.0, 1e-9);
    EXPECT_NEAR(weighted[0].hours, plain[0].hours, 1e-9);
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

    const IndependenceParams indep{};
    const auto plain = build_daily_rates(*storage_, kDay, {});
    const auto weighted =
        build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr, &indep);

    ASSERT_EQ(plain.size(), 1u);
    ASSERT_EQ(weighted.size(), 1u);
    EXPECT_NEAR(plain[0].rate, 2.0, 1e-9);      // (3.0 + 1.0) / 2 — сговор прошёл
    // Дисконт МЯГКИЙ (развилка B3), а не отсечение: потоки пары 15 и 12, по
    // циклу длины 2 вернулось 12, значит R = 12/15 = 0.8; аномалия втрое выше
    // медианы корзины → вес 0.2. Среднее дня = (0.2·3 + 1·1) / (0.2 + 1) = 1.333
    // вместо 2.0: две трети накрутки снято.
    EXPECT_NEAR(weighted[0].rate, 4.0 / 3.0, 1e-3);
    // Часы остаются сырыми: труд-то был, под вопросом его оценка.
    EXPECT_NEAR(weighted[0].hours, 2.0, 1e-9);
    EXPECT_EQ(weighted[0].deals, 2u);
}

TEST_F(IndependenceTest, OneSidedPremiumIsNotDiscounted) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);
    // Дорогая, но ОДНОСТОРОННЯЯ сделка: заказчик у работника ничего не покупал
    // в ответ. Это законная премия (срочность, дефицит) — трогать её нельзя.
    deal(alice_, carol_, 1.0, 3.0, kDay + 100);

    const IndependenceParams indep{};
    const auto plain = build_daily_rates(*storage_, kDay, {});
    const auto weighted =
        build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr, &indep);

    ASSERT_EQ(plain.size(), 1u);
    ASSERT_EQ(weighted.size(), 1u);
    EXPECT_NEAR(weighted[0].rate, plain[0].rate, 1e-9);
    EXPECT_NEAR(weighted[0].rate, 3.0, 1e-9);
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

    // Вчерашняя ставка корзины — 1.0: за день торговала одна лишь пара, так что
    // разница видна в том, насколько ей позволено сдвинуть ставку.
    const std::vector<records::RateEntry> prev = {{"хлебопёк", 1.0, 0.0, 0}};
    const auto s = build_daily_rates(*storage_, kDay, prev, 0.3, 0.1, nullptr,
                                     &short_win);
    const auto l = build_daily_rates(*storage_, kDay, prev, 0.3, 0.1, nullptr,
                                     &long_win);
    ASSERT_EQ(s.size(), 1u);
    ASSERT_EQ(l.size(), 1u);
    // Короткое окно слепо: пара выглядит односторонней, доверие ко дню полное →
    // обычное сглаживание 0.3·3.0 + 0.7·1.0 = 1.6.
    EXPECT_NEAR(s[0].rate, 1.6, 1e-9);
    // Длинное окно видит взаимность: R = 12/15 = 0.8 → вес 0.2, доверие ко дню
    // 0.2, сглаживание 0.3·0.2 = 0.06, и вчерашняя ставка почти не двигается:
    // 0.06·3.0 + 0.94·1.0 = 1.12.
    EXPECT_NEAR(l[0].rate, 1.12, 1e-9);
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

    const auto p = build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr,
                                     &pairs_only);
    const auto r = build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr,
                                     &rings);
    ASSERT_EQ(p.size(), 1u);
    ASSERT_EQ(r.size(), 1u);
    // Парная мера слепа: двухзвенных циклов в кольце нет, вес полный.
    EXPECT_NEAR(p[0].rate, 2.0, 1e-9);
    // Поиск циклов находит треугольник: потоки 15/12/12, узкое место 12,
    // R = 12/15 = 0.8 → вес 0.2, среднее дня (0.2·3 + 1·1)/(0.2 + 1) = 1.333.
    EXPECT_NEAR(r[0].rate, 4.0 / 3.0, 1e-3);
}

// Опора — средневзвешенное по часам ВСЕХ сделок корзины (ратифицировано
// 2026-09-08): сделки не схлопываются по рёбрам. Если большинство честно, то
// каждая сделка есть настоящий труд и обязана считаться, даже когда двое
// работают только друг с другом.
TEST_F(IndependenceTest, DealMeanReferenceCatchesModerateFlooding) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);                       // 10 сделок по цене 1.0
    for (int i = 0; i < 3; ++i) {                    // 6 сделок пары по 3.0
        deal(alice_, carol_, 1.0, 3.0, before);
        deal(carol_, alice_, 1.0, 3.0, before);
    }
    deal(alice_, carol_, 1.0, 3.0, kDay + 100);                     // сговорная
    deal(make_chain(0x71), make_chain(0x81), 1.0, 1.0, kDay + 100); // честная

    const IndependenceParams indep{};
    const auto weighted =
        build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr, &indep);
    ASSERT_EQ(weighted.size(), 1u);
    // Сговорная сделка втрое дороже опоры и полностью кругооборотна — её вес
    // падает, и среднее дня тянется к честной цене, а не к 2.0.
    EXPECT_LT(weighted[0].rate, 1.5);
    EXPECT_NEAR(weighted[0].hours, 2.0, 1e-9);       // часы сырые: труд был
    EXPECT_LT(weighted[0].weighted_hours, weighted[0].hours);
}

// ЧЕМ ЗА ЭТО ПЛАТИМ, честно и в тесте. Опора по рёбрам была неуязвима к числу
// сделок: пара давала одну точку, сколько бы она ни наторговала. Опора по
// сделкам этим свойством не обладает — залив корзину, пара утягивает опору к
// себе и перестаёт выглядеть аномальной.
//
// Замерено на прогоне года (цена ×1.3): при опоре по рёбрам аномалия остаётся
// 1.28x при любой доле сделок; при среднем по сделкам она сползает
// 1.28 → 1.21 (28% сделок) → 1.16 (43%) → 1.11 (61%) → 1.07 (75%).
//
// Это принятая цена, а не недосмотр: заливать корзину значит заявлять ЧАСЫ, а
// часы дороги (economy.md §3) — их подделка требует свидетелей и
// непересекающихся слотов. Кругооборотная половина конъюнкции при этом
// продолжает работать: заливающая пара взаимна по построению.
TEST_F(IndependenceTest, FloodingTheBasketBluntsTheAnomalyHalf) {
    const int64_t before = kDay - 86'400 * 30;
    honest_background(before);                       // 10 честных сделок по 1.0
    for (int i = 0; i < 30; ++i) {                   // 60 сделок пары: 86% корзины
        deal(alice_, carol_, 1.0, 1.3, before);
        deal(carol_, alice_, 1.0, 1.3, before);
    }
    deal(alice_, carol_, 1.0, 1.3, kDay + 100);
    deal(make_chain(0x71), make_chain(0x81), 1.0, 1.0, kDay + 100);

    const IndependenceParams indep{};
    const auto weighted =
        build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr, &indep);
    ASSERT_EQ(weighted.size(), 1u);
    // Опора уехала к цене пары: аномалия сжалась с 1.30 до ≈1.03. Дисконт не
    // исчезает совсем — его удерживает нижний зажим kappa_min = 1.10, — но
    // слабеет резко: уцелевает ≈82% объёма против ≈8% у той же пары в
    // незалитой корзине (CollusiveReciprocalPairIsDiscounted).
    EXPECT_GT(weighted[0].weighted_hours / weighted[0].hours, 0.75);
    EXPECT_LT(weighted[0].weighted_hours, weighted[0].hours);   // но и не ноль
}

// Ниже min_basket_deals отличить аномалию от нормальной цены не по чему.
// Отказываемся судить: наказать честную молодую специальность хуже, чем
// пропустить. Дыра признана и оставлена карте ренты ИР-020.
TEST_F(IndependenceTest, BasketWithTooFewDealsIsNotJudged) {
    const int64_t before = kDay - 86'400 * 30;
    deal(alice_, carol_, 1.0, 1.0, before);          // всего три сделки в корзине
    deal(carol_, alice_, 1.0, 1.0, before);
    deal(alice_, carol_, 1.0, 9.0, kDay + 100);      // вопиюще дорогая и взаимная

    const IndependenceParams indep{};                // min_basket_deals = 4
    const auto weighted =
        build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr, &indep);
    ASSERT_EQ(weighted.size(), 1u);
    EXPECT_NEAR(weighted[0].rate, 9.0, 1e-9);        // не судили вовсе
    EXPECT_NEAR(weighted[0].weighted_hours, weighted[0].hours, 1e-9);
}

// А как только сделок хватает — та же сделка уценивается. Порог отделяет «нечем
// судить» от «судить есть чем», и это видно по одной добавленной сделке.
TEST_F(IndependenceTest, OneMoreDealMakesTheBasketJudgeable) {
    const int64_t before = kDay - 86'400 * 30;
    deal(alice_, carol_, 1.0, 1.0, before);
    deal(carol_, alice_, 1.0, 1.0, before);
    deal(make_chain(0x71), make_chain(0x81), 1.0, 1.0, before);   // четвёртая
    deal(alice_, carol_, 1.0, 9.0, kDay + 100);

    const IndependenceParams indep{};
    const auto weighted =
        build_daily_rates(*storage_, kDay, {}, 0.3, 0.1, nullptr, &indep);
    ASSERT_EQ(weighted.size(), 1u);
    EXPECT_LT(weighted[0].weighted_hours, weighted[0].hours);
}

// ── Облако из сделок (ИР-022): приор без всякого каталога ─────────────────────
//
// Приор спрашивают только о корзине, которая сегодня торговала, а у каждой
// рассчитанной сделки есть разбивка. Значит, точка в облаке у такой корзины есть
// всегда, и объявленные `x` приору не нужны (specialty-axes.md §10).
TEST_F(RatesViewTest, CloudReadOffTheDealsSeedsAThinNewSpecialty) {
    // Повар торговал неделей раньше — и за то же, за что платят пекарю.
    const UserId carol = make_chain(0xC3);
    const Block  spec  = add(carol, records::Specialty{"prof.cook"});
    records::Grade g{};
    g.specialty = ref_to(carol, spec);
    g.level     = 3;
    const auto cook_grade = ref_to(carol, add(carol, g));

    records::WorkRecord wr{};
    wr.agent = cook_grade;
    wr.hours = 4.0;
    const Block work = add(carol, wr);
    records::Acceptance a{};
    a.work        = ref_to(carol, work);
    a.receiver    = bob_.bytes;
    a.hours_raw   = 4.0;
    a.labor_units = 4.8;
    a.axes        = {{test_axis(), 4.8}};
    a.timestamp   = kDay - 7 * 86'400;              // не в сегодняшнем дне
    const Block acc = add(bob_, a);
    records::Transfer t{};
    t.from    = bob_.bytes;
    t.to      = carol.bytes;
    t.origins = { {bob_.bytes, 4.8} };
    t.reason  = ref_to(bob_, acc);
    add(bob_, t);

    settled_deal(bob_, 0.05, 5.0);                  // пекарь: тонко, вчерашней ставки нет

    const auto cloud = build_specialty_cloud(build_specialty_compositions(*storage_),
                                             kDay, kDay, {});
    const std::vector<records::RateEntry> prev = {{"prof.cook", 1.2, 0.0, 0}};
    bool seeded = false;
    for (const auto& r : build_daily_rates(*storage_, kDay, prev, 0.3, 0.1, &cloud))
        if (r.specialty == "хлебопёк") {
            EXPECT_DOUBLE_EQ(r.rate, 1.2);          // ставка соседа по составу цены
            seeded = true;
        }
    EXPECT_TRUE(seeded);
}

// ── Разряд не делит ставку (ИР-022) ───────────────────────────────────────────
//
// Разряд прежде входил в ключ ставки, и час мастера выходил дороже по одному
// лишь званию — сверх того, за что платила сама сделка. Теперь цена расписана
// по осям в сделке, и разряд в ключе оплачивал бы названное второй раз.
TEST_F(RatesViewTest, GradeDoesNotSplitTheRate) {
    settled_deal(bob_, 2.0, 2.0);                   // разряд 3: 1.0 за час

    const Block spec = add(alice_, records::Specialty{"хлебопёк"});
    records::Grade g6{};
    g6.specialty = ref_to(alice_, spec);
    g6.level     = 6;
    const auto master = ref_to(alice_, add(alice_, g6));
    records::WorkRecord wr{};
    wr.agent = master;
    wr.hours = 2.0;
    const Block work = add(alice_, wr);
    records::Acceptance a{};
    a.work        = ref_to(alice_, work);
    a.receiver    = bob_.bytes;
    a.hours_raw   = 2.0;
    a.labor_units = 4.0;                            // разряд 6: 2.0 за час
    a.axes        = {{test_axis(), 4.0}};
    a.timestamp   = kDay + 200;
    const Block acc = add(bob_, a);
    records::Transfer t{};
    t.from    = bob_.bytes;
    t.to      = alice_.bytes;
    t.origins = { {bob_.bytes, 4.0} };
    t.reason  = ref_to(bob_, acc);
    add(bob_, t);

    const auto rates = build_daily_rates(*storage_, kDay, {});
    ASSERT_EQ(rates.size(), 1u);                    // одна ставка, а не две
    EXPECT_EQ(rates[0].specialty, "хлебопёк");
    EXPECT_DOUBLE_EQ(rates[0].rate, 6.0 / 4.0);
    EXPECT_EQ(rates[0].deals, 2u);
}

// Агрегат, опубликованный до v3, несёт по строке на разряд, и после чтения это
// несколько строк одной специальности. Они сливаются в одну — по часам за
// каждой, а если часов нет ни у одной, поровну.
TEST_F(RatesViewTest, PreV3AggregateFoldsItsGradesIntoOneRate) {
    const std::vector<records::RateEntry> by_hours = {
        {"хлебопёк", 1.0, 3.0, 1}, {"хлебопёк", 2.0, 1.0, 1}};
    const auto a = build_daily_rates(*storage_, kDay, by_hours);
    ASSERT_EQ(a.size(), 1u);
    EXPECT_DOUBLE_EQ(a[0].rate, (1.0 * 3.0 + 2.0 * 1.0) / 4.0);

    const std::vector<records::RateEntry> carried = {
        {"хлебопёк", 1.0, 0.0, 0}, {"хлебопёк", 2.0, 0.0, 0}};
    const auto b = build_daily_rates(*storage_, kDay, carried);
    ASSERT_EQ(b.size(), 1u);
    EXPECT_DOUBLE_EQ(b[0].rate, 1.5);
}
