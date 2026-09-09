#include "aggregator/axis_ledger.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

using namespace aggregator;
using namespace blockchain;

namespace {

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

const AxisLedgerRow* row_of(const std::vector<AxisLedgerRow>& v, const records::Ref& a) {
    for (const auto& r : v) if (r.axis == a) return &r;
    return nullptr;
}

} // namespace

// Разбивка ЕСТЬ платёж, а не оценка платежа: сумма часов по осям обязана
// совпасть с ценой сделки. Отсюда и отсутствие невязки — складывать нечего
// сверх того, что назвали.
class AxisLedgerTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    UserId                             worker_ = chain_of(0xA1);
    records::Ref                       grade_ref_{};

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_ledger_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
        const Block spec = add(worker_, records::Specialty{"сварщик"});
        records::Grade g{};
        g.specialty = ref_to(worker_, spec);
        g.level     = 4;
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

    // Ось — запись в цепи; ссылка на неё и есть личность оси.
    records::Ref axis(const std::string& slug, const std::string& ru,
                      const std::string& why, int64_t ts = 1) {
        records::AxisDef d{};
        d.slug = slug; d.ru = ru; d.description = why; d.timestamp = ts;
        return ref_to(chain_of(0xD0), add(chain_of(0xD0), d));
    }

    // Рассчитанная сделка с разбивкой цены прямо в приёмке (records.md §9.5 v4).
    records::Ref settled(const UserId& payer, double hours, double units,
                         std::vector<std::pair<records::Ref, double>> parts = {}) {
        records::WorkRecord wr{};
        wr.agent = grade_ref_;
        wr.hours = hours;
        const Block work = add(worker_, wr);

        records::Acceptance a{};
        a.work        = ref_to(worker_, work);
        a.receiver    = payer.bytes;
        a.hours_raw   = hours;
        a.labor_units = units;
        a.timestamp   = 1000;
        for (auto& [axis, u] : parts) a.axes.push_back({axis, u});
        const Block acc = add(payer, a);

        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = worker_.bytes;
        t.origins = { {payer.bytes, units} };
        t.reason  = ref_to(payer, acc);
        add(payer, t);
        return ref_to(payer, acc);
    }

};

// Доля осей в экономике — факт, а не оценка: столько-то всех оплаченных часов
// ушло на опасность. Ни модели, ни регрессии.
TEST_F(AxisLedgerTest, SharesAreReadStraightOffTheDeals) {
    const auto dng = axis("danger", "Опасность", "риск для самого работника");
    const auto knw = axis("knowledge", "Знание", "порог входа");
    const auto phy = axis("physical", "Физическая нагрузка", "мышечное усилие");
    settled(chain_of(0xB1), 10.0, 14.0, {{dng, 2.8}, {knw, 5.5}, {phy, 5.7}});
    settled(chain_of(0xB2), 10.0, 10.0, {{dng, 1.0}, {knw, 4.0}, {phy, 5.0}});

    const auto led = build_axis_ledger(*storage_);
    ASSERT_EQ(led.size(), 3u);
    const auto* d = row_of(led, dng);
    ASSERT_NE(d, nullptr);
    EXPECT_NEAR(d->units, 3.8, 1e-9);
    EXPECT_NEAR(d->hours, 20.0, 1e-9);
    EXPECT_NEAR(d->per_hour, 3.8 / 20.0, 1e-9);          // 0.19 стч за час работы
    EXPECT_NEAR(d->share, 3.8 / 24.0, 1e-9);             // из 24 оплаченных часов
    EXPECT_EQ(d->deals, 2u);
    EXPECT_EQ(d->chains, 2u);
    EXPECT_EQ(led.front().axis, phy);                    // 10.7 против 9.5 и 3.8
}

// Разбивка, не сходящаяся в цену, описывает какую-то другую сделку и не в счёт.
TEST_F(AxisLedgerTest, BreakdownThatDoesNotAddUpIsIgnored) {
    const auto dng = axis("danger", "Опасность", "риск");
    const auto knw = axis("knowledge", "Знание", "порог входа");
    settled(chain_of(0xB1), 10.0, 14.0, {{dng, 2.8}, {knw, 5.5}});   // 8.3 из 14.0
    EXPECT_TRUE(build_axis_ledger(*storage_).empty());
}

// Разброс — прямо из сказанного, без всякого выбрасывания наблюдений. Широкий
// разброс не значит «модель плоха»: сеть не согласна, что это одна ось.
TEST_F(AxisLedgerTest, SpreadComesStraightFromWhatDealsSaid) {
    const auto dng = axis("danger", "Опасность", "риск");
    const auto phy = axis("physical", "Физическая нагрузка", "усилие");
    settled(chain_of(0xB1), 10.0, 10.0, {{dng, 1.0}, {phy, 9.0}});
    settled(chain_of(0xB2), 10.0, 10.0, {{dng, 5.0}, {phy, 5.0}});
    settled(chain_of(0xB3), 10.0, 10.0, {{dng, 9.0}, {phy, 1.0}});

    const auto led = build_axis_ledger(*storage_);
    const auto* d = row_of(led, dng);
    ASSERT_NE(d, nullptr);
    EXPECT_NEAR(d->per_hour, 0.5, 1e-9);                 // 15 из 30 часов
    EXPECT_NEAR(d->spread, std::sqrt(0.32 / 3.0), 1e-6); // 0.1 / 0.5 / 0.9
}

// Ось без аргумента считается наравне со всеми — но это ВИДНО. Никакой
// «мусорки» протокол не предлагает: что не можешь назвать, то назови.
TEST_F(AxisLedgerTest, UndescribedAxisIsCountedButMarked) {
    const auto dng  = axis("danger", "Опасность", "риск для самого работника");
    const auto slyness = axis("хитрость", "Хитрость", "");   // аргумента нет
    settled(chain_of(0xB1), 10.0, 10.0, {{dng, 4.0}, {slyness, 6.0}});

    const auto defs = build_axis_definitions(*storage_);
    const auto led  = build_axis_ledger(*storage_, &defs);
    ASSERT_EQ(led.size(), 2u);
    EXPECT_TRUE(row_of(led, dng)->described);
    EXPECT_EQ(row_of(led, dng)->label, "Опасность");
    EXPECT_FALSE(row_of(led, slyness)->described);
    EXPECT_NEAR(row_of(led, slyness)->share, 0.6, 1e-9);
}

// Один слаг у разных цепей — две РАЗНЫЕ оси, и никто не выбирает между ними.
// Именно ради этого личность оси — ссылка, а не слаг.
TEST_F(AxisLedgerTest, SameSlugInTwoChainsAreTwoDifferentAxes) {
    records::AxisDef a{};
    a.slug = "danger"; a.ru = "Опасность"; a.description = "риск работника";
    a.timestamp = 100;
    const auto mine = ref_to(chain_of(0xD1), add(chain_of(0xD1), a));
    records::AxisDef b{};
    b.slug = "danger"; b.ru = "Опасность"; b.description = "риск чего угодно";
    b.timestamp = 200;
    const auto theirs = ref_to(chain_of(0xD2), add(chain_of(0xD2), b));
    ASSERT_NE(mine, theirs);

    settled(chain_of(0xB1), 10.0, 10.0, {{mine, 10.0}});
    settled(chain_of(0xB2), 10.0, 10.0, {{theirs, 10.0}});

    const auto defs = build_axis_definitions(*storage_);
    const auto led  = build_axis_ledger(*storage_, &defs);
    ASSERT_EQ(led.size(), 2u);                 // не слились и не «разрешились»
    EXPECT_NEAR(row_of(led, mine)->share, 0.5, 1e-9);
    EXPECT_NEAR(row_of(led, theirs)->share, 0.5, 1e-9);
}

// Ось, на которую сослались, но которой нет ни в одной цепи: считается, но
// видна как неописанная. Заводить оси волен каждый; молчать про них — тоже.
TEST_F(AxisLedgerTest, AxisWithNoDefinitionAtAllIsUndescribed) {
    records::Ref nowhere{};
    nowhere.chain.fill(0xEE);
    nowhere.hash.fill(0xFF);
    settled(chain_of(0xB1), 10.0, 10.0, {{nowhere, 10.0}});

    const auto defs = build_axis_definitions(*storage_);
    const auto led  = build_axis_ledger(*storage_, &defs);
    ASSERT_EQ(led.size(), 1u);
    EXPECT_FALSE(led[0].described);
    EXPECT_TRUE(led[0].label.empty());
}
