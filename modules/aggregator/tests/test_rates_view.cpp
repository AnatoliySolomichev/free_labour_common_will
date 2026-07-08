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
    void settled_deal(const UserId& payer, double hours, double units) {
        records::WorkRecord wr{};
        wr.agent = grade_ref_;
        wr.hours = hours;
        const Block work = add(alice_, wr);

        records::Acceptance a{};
        a.work        = ref_to(alice_, work);
        a.receiver    = payer.bytes;
        a.hours_raw   = hours;
        a.labor_units = units;
        a.timestamp   = kDay + 100;
        const Block acc = add(payer, a);

        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = alice_.bytes;
        t.origins = { {payer.bytes, units} };
        t.reason  = ref_to(payer, acc);
        add(payer, t);
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
    fake.timestamp   = kDay + 100;
    add(bob_, fake);

    // No previous rates → pure day average.
    const auto fresh = build_daily_rates(*storage_, kDay, {});
    ASSERT_EQ(fresh.size(), 1u);
    EXPECT_EQ(fresh[0].specialty, "хлебопёк");
    EXPECT_EQ(fresh[0].level, 3);
    EXPECT_NEAR(fresh[0].rate, 0.8 / 0.6, 1e-9);
    EXPECT_EQ(fresh[0].deals, 2u);

    // With yesterday's 1.25: rate = 0.3*1.3333 + 0.7*1.25 = 1.275, and an
    // untouched specialty carries forward unchanged.
    const std::vector<records::RateEntry> prev = {
        {"хлебопёк", 3, 1.25, 0.0, 0},
        {"кардиохирург", 3, 14.0, 0.0, 0},
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
    const std::vector<records::RateEntry> prev = {{"хлебопёк", 3, 1.25, 0, 0}};
    const auto inherited = build_daily_rates(*storage_, kDay, prev);
    ASSERT_EQ(inherited.size(), 1u);
    EXPECT_DOUBLE_EQ(inherited[0].rate, 1.25);
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
