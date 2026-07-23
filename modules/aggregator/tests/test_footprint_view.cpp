// Economic footprint, layers 2–3 (ИР-010): who took the chain's risk, whose
// work it earned back, and how much of its turnover ever left its own circle.
// The point of the whole thing: an honest chain and a bot farm look identical
// on layer 1 (each declares its own thread) and different here.

#include "aggregator/footprint_view.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>

#include <filesystem>

using namespace aggregator;
using namespace blockchain;

namespace {

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

} // namespace

class FootprintViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_footprint_test_" + std::to_string(++cnt));
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

    // `payer` pays `payee` with paper issued by `issuer`.
    void transfer(const UserId& payer, const UserId& payee,
                  const UserId& issuer, double units) {
        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = payee.bytes;
        t.origins = { {issuer.bytes, units} };
        add(payer, t);
    }

    // `acceptor` accepts work done by `worker`, appraised at `units`.
    void accept_work(const UserId& acceptor, const UserId& worker, double units) {
        records::Acceptance a{};
        a.work.chain  = worker.bytes;
        a.work.hash.fill(static_cast<uint8_t>(next_index_));
        a.receiver    = acceptor.bytes;
        a.hours_raw   = units;
        a.labor_units = units;
        add(acceptor, a);
    }

    ChainFootprint of(const UserId& uid) {
        const auto fp = FootprintView::build(*storage_).chain(uid);
        EXPECT_TRUE(fp.has_value());
        return fp.value_or(ChainFootprint{});
    }
};

// ── Layer 2: who took the risk ───────────────────────────────────────────────

TEST_F(FootprintViewTest, SelfIssuePutsPaperInTheReceiversHands) {
    const auto anna = make_chain(0xA1), vera = make_chain(0xB2);
    transfer(vera, anna, vera, 6.0);        // Vera pays Anna with own paper

    const auto v = of(vera);
    ASSERT_EQ(v.holders.size(), 1u);
    EXPECT_EQ(v.holders[0].chain, anna);
    EXPECT_DOUBLE_EQ(v.holders[0].units, 6.0);
    EXPECT_DOUBLE_EQ(v.paper_outstanding, 6.0);
    // Anna issued nothing: nobody holds her paper.
    EXPECT_TRUE(of(anna).holders.empty());
}

TEST_F(FootprintViewTest, RedemptionAnnihilatesPaperAndCountsAsEarnedBack) {
    const auto anna = make_chain(0xA1), vera = make_chain(0xB2);
    transfer(vera, anna, vera, 6.0);        // Vera owes 6h
    transfer(anna, vera, vera, 4.0);        // Anna buys from Vera with Vera's paper

    const auto v = of(vera);
    ASSERT_EQ(v.holders.size(), 1u);
    EXPECT_DOUBLE_EQ(v.holders[0].units, 2.0);     // 6 issued − 4 returned
    EXPECT_EQ(v.redeemers, 1u);                    // one chain returned her paper
    EXPECT_DOUBLE_EQ(v.redeemed_to_others, 4.0);
}

TEST_F(FootprintViewTest, EndorsementMovesPaperBetweenHolders) {
    const auto anna = make_chain(0xA1), vera = make_chain(0xB2),
               dima = make_chain(0xC3);
    transfer(vera, anna, vera, 6.0);        // Anna holds Vera's paper
    transfer(anna, dima, vera, 6.0);        // Anna passes it to Dima

    const auto v = of(vera);
    ASSERT_EQ(v.holders.size(), 1u);
    EXPECT_EQ(v.holders[0].chain, dima);    // Anna is out, Dima is in
    EXPECT_DOUBLE_EQ(v.paper_outstanding, 6.0);
    EXPECT_EQ(v.redeemers, 0u);             // it never came home
}

TEST_F(FootprintViewTest, AcceptedWorkCountsOnlyFromOtherChains) {
    const auto anna = make_chain(0xA1), vera = make_chain(0xB2),
               dima = make_chain(0xC3);
    accept_work(vera, anna, 6.0);
    accept_work(dima, anna, 3.0);
    accept_work(anna, anna, 99.0);          // self-deal: no signal at all

    const auto a = of(anna);
    EXPECT_EQ(a.acceptors, 2u);
    EXPECT_DOUBLE_EQ(a.labor_outward, 9.0);
}

TEST_F(FootprintViewTest, SpoofedRecordsIgnored) {
    const auto anna = make_chain(0xA1), vera = make_chain(0xB2);
    // Anna writes a transfer claiming Vera is the sender — not Vera's chain.
    records::Transfer t{};
    t.from    = vera.bytes;
    t.to      = anna.bytes;
    t.origins = { {vera.bytes, 100.0} };
    add(anna, t);                            // signed by Anna, claims Vera
    // Anna writes an acceptance claiming Vera received her work.
    records::Acceptance a{};
    a.work.chain  = anna.bytes;
    a.receiver    = vera.bytes;              // ≠ block owner
    a.labor_units = 50.0;
    add(anna, a);

    EXPECT_FALSE(FootprintView::build(*storage_).chain(vera).has_value());
    const auto fp = FootprintView::build(*storage_).chain(anna);
    if (fp) { EXPECT_DOUBLE_EQ(fp->labor_outward, 0.0); }
}

// ── Layer 3: closedness — the screening signal ───────────────────────────────

TEST_F(FootprintViewTest, ClosedFarmVersusOpenChain) {
    // A lazy farm: four chains trading only with each other.
    const auto f1 = make_chain(0xF1), f2 = make_chain(0xF2),
               f3 = make_chain(0xF3), f4 = make_chain(0xF4);
    transfer(f1, f2, f1, 10.0);
    transfer(f2, f3, f2, 10.0);
    transfer(f3, f4, f3, 10.0);
    transfer(f4, f1, f4, 10.0);

    // An open chain: Anna trades with Vera, and Vera also trades outward.
    const auto anna = make_chain(0xA1), vera = make_chain(0xB2),
               dima = make_chain(0xC3), lena = make_chain(0xD4);
    transfer(vera, anna, vera, 10.0);
    transfer(dima, vera, dima, 10.0);
    transfer(lena, dima, lena, 10.0);       // beyond Anna's circle

    const auto farm = of(f1);
    EXPECT_TRUE(farm.has_turnover());
    EXPECT_DOUBLE_EQ(farm.closedness(), 1.0);   // nothing ever leaves
    EXPECT_DOUBLE_EQ(farm.boundary_turnover, 0.0);

    // Anna's circle is {Anna, Vera}; Vera↔Dima crosses out of it.
    const auto a = of(anna);
    EXPECT_GT(a.boundary_turnover, 0.0);
    EXPECT_LT(a.closedness(), 1.0);
}

TEST_F(FootprintViewTest, NoTurnoverIsNotSuspicion) {
    // A chain that only ever had work accepted has no turnover: closedness
    // must not read as 1.0-and-guilty. has_turnover() is what gates the claim.
    const auto anna = make_chain(0xA1), vera = make_chain(0xB2);
    accept_work(vera, anna, 6.0);

    const auto a = of(anna);
    EXPECT_FALSE(a.has_turnover());
    EXPECT_DOUBLE_EQ(a.closedness(), 0.0);
    EXPECT_EQ(a.counterparties, 0u);
}

TEST_F(FootprintViewTest, UnknownChainHasNoFootprint) {
    const auto anna = make_chain(0xA1), stranger = make_chain(0xEE);
    transfer(anna, make_chain(0xB2), anna, 1.0);
    EXPECT_FALSE(FootprintView::build(*storage_).chain(stranger).has_value());
}
