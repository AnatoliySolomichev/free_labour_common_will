#include "aggregator/rent_map.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>

#include <gtest/gtest.h>

#include <filesystem>

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
// КТО её платит: канал схлопывается в одного плательщика, дефицит не меняет
// концентрацию вовсе, потому что опора уезжает вместе со всеми.
class RentMapTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    UserId                             worker_ = chain_of(0xA1);
    records::Ref                       grade_ref_{};

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_rent_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);

        const Block spec = add(worker_, records::Specialty{"портной"});
        records::Grade g{};
        g.specialty = ref_to(worker_, spec);
        g.level     = 5;
        grade_ref_  = ref_to(worker_, add(worker_, g));
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

    // One settled deal at `rate` standard hours per hour, at time `ts`.
    void deal(const UserId& payer, double hours, double rate, int64_t ts) {
        records::WorkRecord wr{};
        wr.agent = grade_ref_;
        wr.hours = hours;
        const Block work = add(worker_, wr);

        records::Acceptance a{};
        a.work        = ref_to(worker_, work);
        a.receiver    = payer.bytes;
        a.hours_raw   = hours;
        a.labor_units = hours * rate;
        a.timestamp   = ts;
        const Block acc = add(payer, a);

        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = worker_.bytes;
        t.origins = { {payer.bytes, hours * rate} };
        t.reason  = ref_to(payer, acc);
        add(payer, t);
    }

    // Eight honest payers trading every month at `rate`.
    void honest_village(double rate) {
        for (int m = 0; m < 12; ++m)
            for (uint8_t p = 0; p < 8; ++p)
                deal(chain_of(static_cast<uint8_t>(0xB0 + p)), 4.0, rate,
                     kFrom + m * 30LL * 86'400 + 3600);
    }

    std::vector<RentRow> map_at(double model_rate, unsigned min_payers = kRentMinPayers) {
        const auto flows = build_basket_flows(*storage_, kFrom, kTo, 1.0);
        std::vector<AxisObservation> obs;
        double h = 0.0, rh = 0.0;
        for (const auto& [key, f] : flows) {
            for (const auto& [who, p] : f.payers) { (void)who; h += p.hours; rh += p.rate_hours; }
            obs.push_back(AxisObservation{key.first, key.second,
                                          h > 0 ? rh / h : 0.0, h, {1.0}});
        }
        return build_rent_map(obs, std::vector<double>(obs.size(), model_rate),
                              flows, min_payers);
    }
};

// Настоящий дефицит: наценку платят ВСЕ. Рента большая, но за неё платят все
// восемь — концентрация ровно такая же, как была бы без наценки.
TEST_F(RentMapTest, ScarcityIsPaidByEveryone) {
    honest_village(1.60);
    const auto rows = map_at(1.00);      // модель ждала 1.00, факт 1.60
    ASSERT_EQ(rows.size(), 1u);
    const auto& r = rows[0];
    EXPECT_TRUE(r.judged);
    EXPECT_NEAR(r.resid, 0.60, 1e-9);
    EXPECT_EQ(r.payers, 8u);
    EXPECT_NEAR(r.rent_payers, 8.0, 1e-6);   // рента размазана по всем восьми
    EXPECT_EQ(r.periods_over, 12u);
    EXPECT_EQ(r.periods_seen, 12u);
}

// Канал: те же восемь честных, плюс один плательщик, который каждый месяц
// платит вдвое. Рента в корзине меньше, чем у дефицита, но платит её ОДИН.
TEST_F(RentMapTest, ChannelCollapsesToOnePayer) {
    honest_village(1.00);
    for (int m = 0; m < 12; ++m)
        deal(chain_of(0xEE), 8.0, 2.00, kFrom + m * 30LL * 86'400 + 7200);

    const auto rows = map_at(1.00);
    ASSERT_EQ(rows.size(), 1u);
    const auto& r = rows[0];
    EXPECT_TRUE(r.judged);
    EXPECT_GT(r.resid, 0.0);
    EXPECT_EQ(r.payers, 9u);
    EXPECT_NEAR(r.rent_payers, 1.0, 1e-6);   // вся рента у одного
    EXPECT_LT(r.rent_payers, 2.0);
}

// Разовый всплеск против стоячей ренты: то же превышение, но один месяц из
// двенадцати. Третье число различает их, первые два — нет.
TEST_F(RentMapTest, OneOffSpikeIsNotAStandingRent) {
    honest_village(1.00);
    deal(chain_of(0xEE), 8.0, 3.00, kFrom + 5 * 30LL * 86'400 + 7200);

    const auto rows = map_at(1.00);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_GT(rows[0].resid, 0.0);
    EXPECT_EQ(rows[0].periods_over, 1u);     // всплеск: один период из двенадцати
    EXPECT_EQ(rows[0].periods_seen, 12u);
}

// Модель угадала — избытка нет ни у кого, и судить нечего. rent_payers = 0
// означает «ренты нет», а не «один плательщик».
TEST_F(RentMapTest, NoRentMeansNoConcentration) {
    honest_village(1.00);
    const auto rows = map_at(1.00);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_NEAR(rows[0].resid, 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(rows[0].rent_payers, 0.0);
}

// Тонкая корзина не судится: деревня с одной пекарней и тремя покупателями
// концентрирована и честна. Остаток печатается, приговор — нет.
TEST_F(RentMapTest, ThinBasketIsNotJudged) {
    for (int m = 0; m < 12; ++m)
        for (uint8_t p = 0; p < 3; ++p)
            deal(chain_of(static_cast<uint8_t>(0xB0 + p)), 4.0, 1.50,
                 kFrom + m * 30LL * 86'400 + 3600);

    const auto rows = map_at(1.00);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].payers, 3u);
    EXPECT_FALSE(rows[0].judged);            // три плательщика — не улика
    EXPECT_DOUBLE_EQ(rows[0].rent_payers, 0.0);
    EXPECT_GT(rows[0].resid, 0.0);           // но сама рента видна
}

// Окно режет по времени приёмки: сделки вне окна не входят ни в ренту, ни в
// счёт периодов.
TEST_F(RentMapTest, WindowExcludesOldDeals) {
    honest_village(1.00);
    deal(chain_of(0xEE), 100.0, 5.00, kFrom - 86'400);   // за день до окна
    const auto rows = map_at(1.00);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].payers, 8u);
    EXPECT_NEAR(rows[0].resid, 0.0, 1e-9);
}
