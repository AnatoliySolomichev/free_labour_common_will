#include "aggregator/economy_view.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>
#include <filesystem>

using namespace aggregator;
using namespace blockchain;

// ── Helpers: signed blocks carrying records ───────────────────────────────────

static UserId make_owner(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

static Block make_record_block(const UserId& owner, BlockIndex index,
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

static records::Ref ref_to(const UserId& chain, const Block& block) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = Crypto::hash_block(block).bytes;
    return r;
}

class EconomyViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_econview_" + std::to_string(++cnt));
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
};

// ── The full scenario: idea, pledges, settlement, labor ───────────────────────

TEST_F(EconomyViewTest, IdeasBoardAndChainDossiers) {
    const UserId alice = make_owner(0xA1);   // author + worker
    const UserId bob   = make_owner(0xB2);   // pledger + payer
    const UserId carol = make_owner(0xC3);   // second pledger, revokes

    // Alice publishes an idea; Bob copies it, Carol reacts +5.
    records::Concept concept{"ветрогенератор", {}};
    const Block idea = add(alice, concept);
    const auto  idea_ref = ref_to(alice, idea);
    add(bob,   records::Copy{idea_ref});
    add(carol, records::Reaction{idea_ref, +5});

    // Bob pledges 4h and settles 3h of it; Carol pledges 2h and revokes.
    records::Pledge bp{};
    bp.target = idea_ref;
    bp.units  = 4;
    const Block bob_pledge = add(bob, bp);

    records::Pledge cp{};
    cp.target = idea_ref;
    cp.units  = 2;
    const Block carol_pledge = add(carol, cp);
    add(carol, records::PledgeRevoke{ref_to(carol, carol_pledge), 10});

    // Alice's work appraised by Bob at 4.2 labor-h.
    records::WorkRecord work{};
    work.agent  = idea_ref;   // any ref; agent grade is irrelevant here
    work.action = "сборка";
    work.hours  = 4;
    const Block work_block = add(alice, work);
    records::Acceptance acc{};
    acc.work        = ref_to(alice, work_block);
    acc.receiver    = bob.bytes;
    acc.quality     = "пройдено";
    acc.hours_raw   = 4;
    acc.labor_units = 4.2;
    const Block acc_block = add(bob, acc);

    // Transfer v4 (§11.1): `reason` says WHAT is paid for (the acceptance),
    // `settles` says WHICH promise this closes. A pledge is settled only by
    // `settles` — `reason` must name the acceptance, so it never could.
    records::Transfer settle{};
    settle.from    = bob.bytes;
    settle.to      = alice.bytes;
    settle.origins = { {bob.bytes, 3.0} };            // self-issue
    settle.reason  = ref_to(bob, acc_block);
    settle.settles = ref_to(bob, bob_pledge);
    add(bob, settle);

    const auto view = EconomyView::build(*storage_, /*now=*/1000);

    // Ideas board: one idea, active = 4−3 (Bob) + 0 (Carol revoked) = 1.
    ASSERT_EQ(view.ideas().size(), 1u);
    const auto& board = view.ideas()[0];
    EXPECT_EQ(board.idea_hash.bytes, idea_ref.hash);
    EXPECT_EQ(board.text, "ветрогенератор");
    EXPECT_DOUBLE_EQ(board.pledged_active, 1.0);
    EXPECT_DOUBLE_EQ(board.pledged_settled, 3.0);
    EXPECT_EQ(board.pledgers, 2u);
    EXPECT_EQ(board.copies, 1u);
    EXPECT_EQ(board.reaction_sum, 5);

    // Bob's dossier: debt 3 (self-issued, nothing redeemed), 1 active pledge.
    const auto bob_view = view.chain(bob);
    ASSERT_TRUE(bob_view.has_value());
    EXPECT_DOUBLE_EQ(bob_view->debt(), 3.0);
    EXPECT_DOUBLE_EQ(bob_view->spent, 3.0);
    EXPECT_EQ(bob_view->pledges_active, 1u);
    EXPECT_EQ(bob_view->pledges_revoked, 0u);

    // Carol: one revoked pledge, no flows.
    const auto carol_view = view.chain(carol);
    ASSERT_TRUE(carol_view.has_value());
    EXPECT_EQ(carol_view->pledges_revoked, 1u);
    EXPECT_DOUBLE_EQ(carol_view->debt(), 0.0);

    // Alice: received 3h, her work accepted once at 4.2 labor-h.
    const auto alice_view = view.chain(alice);
    ASSERT_TRUE(alice_view.has_value());
    EXPECT_DOUBLE_EQ(alice_view->received, 3.0);
    EXPECT_EQ(alice_view->works_accepted, 1u);
    EXPECT_DOUBLE_EQ(alice_view->labor_appraised, 4.2);
}

TEST_F(EconomyViewTest, ExpiredPledgeAndSpoofedTransferIgnored) {
    const UserId dave = make_owner(0xD4);
    const UserId eve  = make_owner(0xEE);

    records::Pledge expired{};
    expired.target.hash.fill(0x77);
    expired.units   = 5;
    expired.expires = 100;                     // now=1000 → expired
    add(dave, expired);

    // Eve writes a transfer claiming to be FROM Dave — spoof, must not count.
    records::Transfer spoof{};
    spoof.from    = dave.bytes;
    spoof.to      = eve.bytes;
    spoof.origins = { {dave.bytes, 99.0} };
    add(eve, spoof);

    const auto view = EconomyView::build(*storage_, /*now=*/1000);
    const auto dave_view = view.chain(dave);
    ASSERT_TRUE(dave_view.has_value());
    EXPECT_EQ(dave_view->pledges_expired, 1u);
    EXPECT_EQ(dave_view->pledges_active, 0u);
    EXPECT_DOUBLE_EQ(dave_view->debt(), 0.0);   // spoof did not create debt
    ASSERT_EQ(view.ideas().size(), 1u);
    EXPECT_DOUBLE_EQ(view.ideas()[0].pledged_active, 0.0);
}
