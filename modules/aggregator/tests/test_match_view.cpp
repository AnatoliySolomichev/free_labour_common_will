#include "aggregator/match_view.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>
#include <filesystem>

using namespace aggregator;
using namespace blockchain;

namespace {

UserId make_owner(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

records::Concept fact(const std::string& text,
                      const std::vector<std::string>& tags) {
    records::Concept c;
    c.text = text;
    c.tags = tags;
    return c;
}

// What the catalog supplies (records.md §8.7).
const ClosedBy CLOSED_BY = {
    {"need.electrical",       {"prof.electrician"}},
    {"need.food",             {"prof.cook"}},
    {"need.clothing",         {"prof.tailor"}},
    {"need.appliance-repair", {"prof.appliance-repair"}},
    {"need.accounting",       {"prof.accountant"}},
};

} // namespace

class MatchViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_matchview_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    void add(const UserId& owner, const records::Record& rec) {
        Block b{};
        b.address           = {owner, 0x7FFF'FFFFu, next_index_++};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 1000;
        b.type              = BlockType::DATA;
        b.payload           = records::Codec::encode(rec);
        b.signature         = Signature::null();
        const auto    bytes = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        EXPECT_TRUE(storage_->add_block(b));
    }
};

// ── Distance ─────────────────────────────────────────────────────────────────

TEST_F(MatchViewTest, DistanceIsHaversine) {
    // Two points ~1.8 km apart in Moscow.
    const double d = MatchView::distance_km("55.75,37.62", "55.76,37.60");
    EXPECT_GT(d, 1.0);
    EXPECT_LT(d, 3.0);

    EXPECT_DOUBLE_EQ(MatchView::distance_km("55.75,37.62", "55.75,37.62"), 0.0);
    EXPECT_LT(MatchView::distance_km("мусор", "55.75,37.62"), 0);
}

// ── The core: a need is closed by a reachable skill ───────────────────────────

TEST_F(MatchViewTest, MatchesNeedToNearbySkill) {
    const UserId anna = make_owner(0xA1);   // electrician
    const UserId vera = make_owner(0xB2);

    add(anna, fact("Электромонтаж", {"kind:skill", "cat:prof.electrician",
                                     "geo:55.75,37.62", "r:30", "grade:4"}));
    add(vera, fact("Заменить проводку", {"kind:need", "cat:need.electrical",
                                         "geo:55.76,37.60", "r:10",
                                         "urgency:high"}));

    const auto view = MatchView::build(*storage_, CLOSED_BY);
    ASSERT_EQ(view.needs().size(), 1u);

    const NeedMatch& m = view.needs()[0];
    EXPECT_EQ(m.slug,    "need.electrical");
    EXPECT_EQ(m.urgency, "high");
    ASSERT_EQ(m.candidates.size(), 1u);
    EXPECT_EQ(m.candidates[0].chain, anna);
    EXPECT_EQ(m.candidates[0].grade, "4");
    EXPECT_GT(m.candidates[0].distance_km, 0.0);
    EXPECT_LT(m.candidates[0].distance_km, 3.0);
    EXPECT_TRUE(view.deficits().empty());
}

// Distance beats good intentions: too far is no match.
TEST_F(MatchViewTest, OutOfReachIsNotAMatch) {
    const UserId anna = make_owner(0xA1);
    const UserId vera = make_owner(0xB2);

    // Anna serves 5 km around Moscow; Vera is in Saint Petersburg.
    add(anna, fact("Электромонтаж", {"kind:skill", "cat:prof.electrician",
                                     "geo:55.75,37.62", "r:5"}));
    add(vera, fact("Заменить проводку", {"kind:need", "cat:need.electrical",
                                         "geo:59.94,30.31", "r:5"}));

    const auto view = MatchView::build(*storage_, CLOSED_BY);
    ASSERT_EQ(view.needs().size(), 1u);
    EXPECT_TRUE(view.needs()[0].candidates.empty());
    ASSERT_EQ(view.deficits().size(), 1u);
    EXPECT_EQ(view.deficits()[0].need_slug, "need.electrical");
}

// Remote work ignores geography — that is the point of remote:yes / r:global.
TEST_F(MatchViewTest, RemoteSkillReachesAnyDistance) {
    const UserId grigoriy = make_owner(0xA1);
    const UserId darya    = make_owner(0xB2);

    add(grigoriy, fact("Бухгалтерия удалённо", {"kind:skill", "cat:prof.accountant",
                                                "remote:yes", "r:global"}));
    add(darya, fact("Помощь с налогами", {"kind:need", "cat:need.accounting",
                                          "geo:59.94,30.31", "r:5"}));

    const auto view = MatchView::build(*storage_, CLOSED_BY);
    ASSERT_EQ(view.needs().size(), 1u);
    ASSERT_EQ(view.needs()[0].candidates.size(), 1u);
    EXPECT_EQ(view.needs()[0].candidates[0].chain, grigoriy);
    EXPECT_LT(view.needs()[0].candidates[0].distance_km, 0);   // not applicable
}

// Missing data must never hide a person: an unstated coordinate or radius means
// "no constraint", not "no match".
TEST_F(MatchViewTest, MissingGeoDoesNotHideAnyone) {
    const UserId anna = make_owner(0xA1);
    const UserId vera = make_owner(0xB2);

    add(anna, fact("Электромонтаж", {"kind:skill", "cat:prof.electrician"}));
    add(vera, fact("Заменить проводку", {"kind:need", "cat:need.electrical",
                                         "geo:55.76,37.60", "r:1"}));

    const auto view = MatchView::build(*storage_, CLOSED_BY);
    ASSERT_EQ(view.needs().size(), 1u);
    ASSERT_EQ(view.needs()[0].candidates.size(), 1u);
    EXPECT_EQ(view.needs()[0].candidates[0].chain, anna);
}

// ── Deficit and retraining ───────────────────────────────────────────────────

TEST_F(MatchViewTest, DeficitNamesWhoCouldGrowIntoIt) {
    const UserId boris = make_owner(0xA1);
    const UserId vera  = make_owner(0xB2);

    add(boris, fact("Сломалась стиралка", {"kind:need", "cat:need.appliance-repair"}));
    // Boris himself is willing to learn the very trade that is missing.
    add(boris, fact("Готов освоить", {"kind:aspiration", "cat:prof.appliance-repair",
                                      "retrain:yes"}));
    add(vera,  fact("Готова переучиться", {"kind:aspiration", "cat:prof.accountant",
                                           "retrain:yes"}));

    const auto view = MatchView::build(*storage_, CLOSED_BY);
    ASSERT_EQ(view.deficits().size(), 1u);

    const Deficit& d = view.deficits()[0];
    EXPECT_EQ(d.need_slug, "need.appliance-repair");
    ASSERT_EQ(d.professions.size(), 1u);
    EXPECT_EQ(d.professions[0], "prof.appliance-repair");

    // The seeker is NOT excluded: someone learning the missing trade serves the
    // whole circle, their own need included.
    ASSERT_EQ(d.aspiring.size(), 1u);
    EXPECT_EQ(d.aspiring[0], boris);

    // Anyone open to retraining at all is a fallback answer to scarcity.
    EXPECT_EQ(d.willing.size(), 2u);
}

// ── Rings: help that closes on itself ────────────────────────────────────────

TEST_F(MatchViewTest, FindsRingOfThree) {
    const UserId anna  = make_owner(0xA1);   // electrician, needs a cook
    const UserId darya = make_owner(0xB2);   // cook, needs clothes
    const UserId vera  = make_owner(0xC3);   // tailor, needs an electrician

    add(anna,  fact("Электромонтаж",  {"kind:skill", "cat:prof.electrician"}));
    add(anna,  fact("Нужен повар",    {"kind:need",  "cat:need.food"}));
    add(darya, fact("Повар",          {"kind:skill", "cat:prof.cook"}));
    add(darya, fact("Нужно сшить",    {"kind:need",  "cat:need.clothing"}));
    add(vera,  fact("Пошив одежды",   {"kind:skill", "cat:prof.tailor"}));
    add(vera,  fact("Нужна проводка", {"kind:need",  "cat:need.electrical"}));

    const auto view = MatchView::build(*storage_, CLOSED_BY);

    // Every need is met — nobody is left owing an outsider.
    for (const auto& m : view.needs()) EXPECT_FALSE(m.candidates.empty());
    EXPECT_TRUE(view.deficits().empty());

    ASSERT_EQ(view.rings().size(), 1u);
    const auto& ring = view.rings()[0].chains;
    ASSERT_EQ(ring.size(), 3u);
    // Canonical rotation: the ring starts at its smallest member.
    EXPECT_EQ(ring[0], anna);
    EXPECT_EQ(std::set<UserId>(ring.begin(), ring.end()),
              (std::set<UserId>{anna, darya, vera}));
}

TEST_F(MatchViewTest, FindsMutualPairAndNoFalseRings) {
    const UserId boris    = make_owner(0xA1);  // mechanic, needs accounting
    const UserId grigoriy = make_owner(0xB2);  // accountant, needs car repair
    const UserId lonely   = make_owner(0xC3);  // needs a cook; nobody cooks

    add(boris,    fact("Автослесарь",   {"kind:skill", "cat:prof.auto-mechanic"}));
    add(boris,    fact("Нужны налоги",  {"kind:need",  "cat:need.accounting"}));
    add(grigoriy, fact("Бухгалтерия",   {"kind:skill", "cat:prof.accountant"}));
    add(grigoriy, fact("Ремонт машины", {"kind:need",  "cat:need.vehicle-repair"}));
    add(lonely,   fact("Нужен повар",   {"kind:need",  "cat:need.food"}));

    ClosedBy cb = CLOSED_BY;
    cb["need.vehicle-repair"] = {"prof.auto-mechanic"};

    const auto view = MatchView::build(*storage_, cb);

    ASSERT_EQ(view.rings().size(), 1u);
    EXPECT_EQ(view.rings()[0].chains.size(), 2u);   // a mutual pair is a ring of 2

    ASSERT_EQ(view.deficits().size(), 1u);
    EXPECT_EQ(view.deficits()[0].need_slug, "need.food");
}

// A need whose slug the catalog does not map is a deficit, not a crash.
TEST_F(MatchViewTest, UnmappedNeedBecomesDeficit) {
    const UserId someone = make_owner(0xA1);
    add(someone, fact("Нужно что-то странное", {"kind:need", "cat:need.leisure"}));

    const auto view = MatchView::build(*storage_, CLOSED_BY);
    ASSERT_EQ(view.deficits().size(), 1u);
    EXPECT_EQ(view.deficits()[0].need_slug, "need.leisure");
    EXPECT_TRUE(view.deficits()[0].professions.empty());
}
