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
// (records.md §11.11); модели здесь нет, поэтому миру нужна вторая корзина:
// без остальной сети сравнивать корзину не с чем.
class RentMapTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    UserId                             worker_ = chain_of(0xA1);
    records::Ref                       tailor_{};      // исследуемая корзина
    records::Ref                       seamstress_{};  // остальная сеть

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_rent_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
        tailor_     = grade_of("портной", 5);
        seamstress_ = grade_of("швея", 3);
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

    records::Ref grade_of(const std::string& specialty, uint8_t level) {
        const Block spec = add(worker_, records::Specialty{specialty});
        records::Grade g{};
        g.specialty = ref_to(worker_, spec);
        g.level     = level;
        return ref_to(worker_, add(worker_, g));
    }

    // Рассчитанная сделка по цене `rate` за час. `named` < 0 — разбивка равна
    // цене; иначе разбивка называет `named` часов и с ценой НЕ сходится.
    void deal(const records::Ref& grade, const UserId& payer, double hours,
              double rate, int64_t ts, double named = -1.0) {
        records::WorkRecord wr{};
        wr.agent = grade;
        wr.hours = hours;
        const Block work = add(worker_, wr);

        records::Acceptance a{};
        a.work        = ref_to(worker_, work);
        a.receiver    = payer.bytes;
        a.hours_raw   = hours;
        a.labor_units = hours * rate;
        a.axes        = {{test_axis(), named < 0.0 ? hours * rate : named}};
        a.timestamp   = ts;
        const Block acc = add(payer, a);

        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = worker_.bytes;
        t.origins = { {payer.bytes, hours * rate} };
        t.reason  = ref_to(payer, acc);
        add(payer, t);
    }

    // `payers` честных плательщиков, каждый месяц по 4 часа по цене `rate`.
    void village(const records::Ref& grade, double rate, uint8_t payers = 8) {
        for (int m = 0; m < 12; ++m)
            for (uint8_t p = 0; p < payers; ++p)
                deal(grade, chain_of(static_cast<uint8_t>(0xB0 + p)), 4.0, rate,
                     kFrom + m * 30LL * 86'400 + 3600);
    }

    std::vector<RentRow> rent(unsigned min_payers = kRentMinPayers) {
        return build_rent_map(build_rent_input(*storage_, kFrom, kTo), min_payers);
    }

    static const RentRow* row(const std::vector<RentRow>& v, const std::string& slug) {
        for (const auto& r : v) if (r.slug == slug) return &r;
        return nullptr;
    }
};

// Настоящий дефицит: наценку платят ВСЕ. Рента есть, но за неё платят все
// восемь поровну. Сеть: швеи по 1.0, портные по 1.6 → цена часа оси 1.3.
TEST_F(RentMapTest, ScarcityIsPaidByEveryone) {
    village(seamstress_, 1.0);
    village(tailor_, 1.6);
    const auto rows = rent();
    const auto* r = row(rows, "портной");
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
// месяц платит вдвое. Рента у корзины меньше, чем у дефицита, но платит её ОДИН.
TEST_F(RentMapTest, ChannelCollapsesToOnePayer) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    for (int m = 0; m < 12; ++m)
        deal(tailor_, chain_of(0xEE), 8.0, 2.0, kFrom + m * 30LL * 86'400 + 7200);

    const auto rows = rent();
    const auto* r = row(rows, "портной");
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
    const auto* r = row(rows, "портной");
    ASSERT_NE(r, nullptr);
    EXPECT_GT(r->resid, 0.0);
    EXPECT_EQ(r->periods_over, 1u);            // всплеск: один период из двенадцати
    EXPECT_EQ(r->periods_seen, 12u);
}

// Корзина платит ровно сетевую цену — превышения нет ни у кого. rent_payers = 0
// означает «ренты нет», а не «один плательщик».
TEST_F(RentMapTest, NoRentMeansNoConcentration) {
    village(seamstress_, 1.0);
    village(tailor_, 1.0);
    const auto rows = rent();
    const auto* r = row(rows, "портной");
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->judged);
    EXPECT_NEAR(r->resid, 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(r->rent_payers, 0.0);
}

// Тонкая корзина не судится: деревня с тремя покупателями концентрирована и
// честна. Остаток печатается, приговор — нет.
TEST_F(RentMapTest, ThinBasketIsNotJudged) {
    village(seamstress_, 1.0);
    village(tailor_, 1.5, 3);
    const auto rows = rent();
    const auto* r = row(rows, "портной");
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
    const auto* r = row(rows, "портной");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->payers, 8u);
    EXPECT_NEAR(r->resid, 0.0, 1e-9);
}

// Опора — СЕТЬ, а не сама корзина. Если вся сеть — одна корзина, платящая
// 1.6, превышения нет: сравнивать не с чем. Это и отличает опору по книге осей
// от опоры по собственной средней корзины, в которой дефицит растворился бы.
TEST_F(RentMapTest, ReferenceIsTheNetworkNotTheBasket) {
    village(tailor_, 1.6);
    const auto rows = rent();
    const auto* r = row(rows, "портной");
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
    const auto* r = row(rows, "портной");
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->payers, 8u);                  // сделки 0xEE как не было
    EXPECT_NEAR(r->resid, 0.0, 1e-9);
}
