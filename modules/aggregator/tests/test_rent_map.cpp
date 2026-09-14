#include "aggregator/rent_map.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>

#include <gtest/gtest.h>

#include <filesystem>

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

constexpr int64_t kFrom = 86'400LL * 100;
constexpr int64_t kTo   = kFrom + 365LL * 86'400;

UserId chain_of(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

records::Ref ref_to(const UserId& chain, const Block& block) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = Crypto::hash_block(block).bytes;
    return r;
}

} // namespace

// Один и тот же остаток — два разных зверя. Различает не величина ренты, а то,
// КТО её платит: канал схлопывается в одного плательщика, дефицит размазан по
// всем. Опора — сетевая цена часа каждой оси, прочитанная из самих сделок
// (records.md §11.11); модели здесь нет, поэтому миру нужен второй работник: без
// остальной сети сравнивать не с чем. Строки — по работникам (ИР-022 шаг 11):
// специальностей больше нет, а канал — это ровно «один плательщик переплачивает
// одному работнику».
class RentMapTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    UserId                             tailor_     = chain_of(0xA1);  // исследуемый
    UserId                             seamstress_ = chain_of(0xA2);  // остальная сеть

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_rent_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    Block add(const UserId& owner, const records::Record& rec) {
        Block b{};
        b.address           = {owner, 0x7FFF'FFFFu, next_index_++};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 0;
        b.type              = BlockType::DATA;
        b.payload           = records::Codec::encode(rec);
        b.signature         = Signature::null();
        const auto bytes    = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        EXPECT_TRUE(storage_->add_block(b));
        return b;
    }

    // Рассчитанная сделка работника по цене `rate` за час. `named` < 0 — разбивка
    // равна цене; иначе разбивка называет `named` часов и с ценой НЕ сходится.
    void deal(const UserId& worker, const UserId& payer, double hours,
              double rate, int64_t ts, double named = -1.0,
              const std::string& title = "") {
        records::WorkRecord wr{};
        wr.hours = hours;
        wr.title = !title.empty() ? title : worker == tailor_ ? "портной" : "швея";
        wr.customer = payer.bytes;   // на заказ плательщика (records.md §9.4)
        const Block work = add(worker, wr);

        records::Acceptance a{};
        a.work        = ref_to(worker, work);
        a.hours_raw   = hours;
        a.labor_units = hours * rate;
        a.axes        = {{test_axis(), named < 0.0 ? hours * rate : named}};

        // Приёмка — внутри своей оплаты (records.md §9.5, ИР-023).
        records::Transfer t{};
        t.from      = payer.bytes;
        t.to        = worker.bytes;
        t.origins   = { {payer.bytes, hours * rate} };
        t.accept    = a;
        t.timestamp = ts;
        add(payer, t);
    }

    // `payers` честных плательщиков, каждый месяц по 4 часа по цене `rate`.
    void village(const UserId& worker, double rate, uint8_t payers = 8) {
        for (int m = 0; m < 12; ++m)
            for (uint8_t p = 0; p < payers; ++p)
                deal(worker, chain_of(static_cast<uint8_t>(0xB0 + p)), 4.0, rate,
                     kFrom + m * 30LL * 86'400 + 3600);
    }

    std::vector<RentRow> rent(unsigned min_payers = kRentMinPayers) {
        return build_rent_map(build_rent_input(*storage_, kFrom, kTo), min_payers);
    }

    static const RentRow* row(const std::vector<RentRow>& v, const UserId& worker) {
        for (const auto& r : v) if (r.worker == worker.bytes) return &r;
        return nullptr;
    }
};

// Настоящий дефицит: наценку платят ВСЕ. Рента есть, но за неё платят все
// восемь поровну. Сеть: швее по 1.0, портному по 1.6 → цена часа оси 1.3.
TEST_F(RentMapTest, ScarcityIsPaidByEveryone) {
    village(seamstress_, 1.0);
    village(tailor_, 1.6);
    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->judged);
    EXPECT_NEAR(r->expected, 1.3, 1e-9);
    EXPECT_NEAR(r->resid, 0.3, 1e-9);
    EXPECT_EQ(r->payers, 8u);
    EXPECT_NEAR(r->rent_payers, 8.0, 1e-6);   // рента размазана по всем восьми
    EXPECT_EQ(r->periods_over, 12u);
    EXPECT_EQ(r->periods_seen, 12u);
}

// Канал: восемь честных по сетевой цене плюс один плательщик, который каждый
// месяц платит вдвое. Рента меньше, чем у дефицита, но платит её ОДИН.
TEST_F(RentMapTest, ChannelCollapsesToOnePayer) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    for (int m = 0; m < 12; ++m)
        deal(tailor_, chain_of(0xEE), 8.0, 2.0, kFrom + m * 30LL * 86'400 + 7200);

    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->judged);
    EXPECT_GT(r->resid, 0.0);
    EXPECT_EQ(r->payers, 9u);
    EXPECT_NEAR(r->rent_payers, 1.0, 1e-6);   // вся рента у одного
}

// Разовый всплеск против стоячей ренты: превышение есть, но один период из
// двенадцати. Третье число различает их, первые два — нет.
TEST_F(RentMapTest, OneOffSpikeIsNotAStandingRent) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    deal(tailor_, chain_of(0xEE), 8.0, 3.0, kFrom + 5 * 30LL * 86'400 + 7200);

    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_GT(r->resid, 0.0);
    EXPECT_EQ(r->periods_over, 1u);            // всплеск: один период из двенадцати
    EXPECT_EQ(r->periods_seen, 12u);
}

// Работнику платят ровно сетевую цену — превышения нет ни у кого. rent_payers = 0
// означает «ренты нет», а не «один плательщик».
TEST_F(RentMapTest, NoRentMeansNoConcentration) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->judged);
    EXPECT_NEAR(r->resid, 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(r->rent_payers, 0.0);
}

// Работник с тремя заказчиками не судится: деревня с тремя покупателями
// концентрирована и честна. Остаток печатается, приговор — нет.
TEST_F(RentMapTest, ThinWorkerIsNotJudged) {
    village(seamstress_, 1.0);
    village(tailor_, 1.5, 3);
    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->payers, 3u);
    EXPECT_FALSE(r->judged);                   // три плательщика — не улика
    EXPECT_DOUBLE_EQ(r->rent_payers, 0.0);
    EXPECT_GT(r->resid, 0.0);                  // но сама рента видна
}

// Окно режет по времени приёмки: сделки вне окна не входят ни в ренту, ни в
// счёт периодов.
TEST_F(RentMapTest, WindowExcludesOldDeals) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    deal(tailor_, chain_of(0xEE), 100.0, 5.0, kFrom - 86'400);   // за день до окна
    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->payers, 8u);
    EXPECT_NEAR(r->resid, 0.0, 1e-9);
}

// Опора — СЕТЬ, а не сам работник. Если вся сеть — один работник, которому
// платят 1.6, превышения нет: сравнивать не с чем. Это и отличает опору по книге
// осей от опоры по собственной средней, в которой дефицит растворился бы.
TEST_F(RentMapTest, ReferenceIsTheNetworkNotTheWorker) {
    village(tailor_, 1.6);
    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_NEAR(r->expected, 1.6, 1e-9);
    EXPECT_NEAR(r->resid, 0.0, 1e-9);
}

// Разбивка, не сходящаяся с ценой, описывает другую сделку — в ренту не входит,
// как не входит и в книгу осей.
TEST_F(RentMapTest, BreakdownThatDoesNotAddUpIsLeftOut) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    deal(tailor_, chain_of(0xEE), 8.0, 3.0, kFrom + 3600, 5.0);  // 24 цены, 5 названо
    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->payers, 8u);                  // сделки 0xEE как не было
    EXPECT_NEAR(r->resid, 0.0, 1e-9);
}

// У строки — название последней работы вольным текстом: люди читают карту по
// названиям, а не по ключам цепей. По названию ничего не считается.
TEST_F(RentMapTest, RowCarriesTheLatestTitle) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    deal(tailor_, chain_of(0xB0), 4.0, 1.0, kFrom + 11 * 30LL * 86'400 + 9000, -1.0,
         "портной, сюртуки");
    const auto rows = rent();
    const auto* r = row(rows, tailor_);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->title, "портной, сюртуки");
    EXPECT_EQ(row(rows, seamstress_)->title, "швея");
}
