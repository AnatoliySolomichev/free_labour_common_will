#include "aggregator/profile_view.h"

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

static Block make_record_block(const UserId& owner, NodeIndex node,
                               BlockIndex index, const records::Record& rec) {
    Block b{};
    b.address           = {owner, node, index};
    b.prev_hash         = Hash::zero();
    b.timestamp_claimed = static_cast<Timestamp>(index) * 1000LL;
    b.type              = BlockType::DATA;
    b.payload           = records::Codec::encode(rec);
    b.signature         = Signature::null();
    const auto    bytes = Serializer::encode(b);
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

// A profile fact is an ordinary Concept — the convention lives in the tags.
static records::Concept fact(const std::string&              text,
                             const std::vector<std::string>& tags) {
    records::Concept c;
    c.text = text;
    c.tags = tags;
    return c;
}

class ProfileViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_profview_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    Block add(const UserId& owner, const records::Record& rec,
              NodeIndex node = 0x7FFF'FFFFu) {
        const Block b = make_record_block(owner, node, next_index_++, rec);
        EXPECT_TRUE(storage_->add_block(b));
        return b;
    }
};

// ── Facets, and the attribute-agnostic tag pass-through ───────────────────────

TEST_F(ProfileViewTest, FacetsGroupedAndUnknownTagsPassThrough) {
    const UserId alice = make_owner(0xA1);

    add(alice, fact("Электромонтаж, 8 лет",
                    {"kind:skill", "cat:prof.electrician", "retrain:no",
                     "geo:55.75,37.62", "r:30"}));
    add(alice, fact("Нужно заменить проводку",
                    {"kind:need", "cat:need.electrical", "horizon:now"}));
    add(alice, fact("Хочу освоить сварку",
                    {"kind:aspiration", "cat:prof.welder"}));
    add(alice, fact("Просто мысль о проекте", {"проект"}));  // no kind: → not a fact

    const auto view    = ProfileView::build(*storage_);
    const auto profile = view.chain(alice);
    ASSERT_TRUE(profile.has_value());
    EXPECT_EQ(profile->facts.size(), 3u);   // the plain idea is not a profile fact

    const auto skills = profile->by_facet(ProfileFacet::Skill);
    ASSERT_EQ(skills.size(), 1u);
    EXPECT_EQ(skills[0]->slug, "prof.electrician");
    EXPECT_EQ(skills[0]->text, "Электромонтаж, 8 лет");
    EXPECT_FALSE(skills[0]->closed);

    // Attributes the view does not model (geo, radius) still reach the consumer
    // verbatim — new characteristics need no change here.
    EXPECT_EQ(skills[0]->tags.size(), 5u);
    EXPECT_EQ(ProfileView::tag_value(skills[0]->tags, "geo:"), "55.75,37.62");
    EXPECT_EQ(ProfileView::tag_value(skills[0]->tags, "r:"),   "30");
    EXPECT_EQ(ProfileView::tag_value(skills[0]->tags, "horizon:"), "");

    EXPECT_EQ(profile->by_facet(ProfileFacet::Need).size(),       1u);
    EXPECT_EQ(profile->by_facet(ProfileFacet::Aspiration).size(), 1u);
    EXPECT_TRUE(profile->by_facet(ProfileFacet::Hobby).empty());
}

// ── The matching primitive: demand against supply ─────────────────────────────

TEST_F(ProfileViewTest, BySlugStitchesDemandToSupply) {
    const UserId alice = make_owner(0xA1);   // electrician
    const UserId bob   = make_owner(0xB2);   // needs one
    const UserId carol = make_owner(0xC3);   // electrician too

    add(alice, fact("Электромонтаж",     {"kind:skill", "cat:prof.electrician"}));
    add(carol, fact("Электрик, 4 разряд", {"kind:skill", "cat:prof.electrician"}));
    add(bob,   fact("Нужен электрик",     {"kind:need",  "cat:need.electrical"}));
    // An aspiration is not supply — Bob wants to learn it, he cannot do it yet.
    add(bob,   fact("Хочу выучиться на электрика",
                    {"kind:aspiration", "cat:prof.electrician"}));

    const auto view = ProfileView::build(*storage_);

    const auto suppliers = view.by_slug(ProfileFacet::Skill, "prof.electrician");
    ASSERT_EQ(suppliers.size(), 2u);
    EXPECT_EQ(suppliers[0], alice);
    EXPECT_EQ(suppliers[1], carol);

    const auto seekers = view.by_slug(ProfileFacet::Need, "need.electrical");
    ASSERT_EQ(seekers.size(), 1u);
    EXPECT_EQ(seekers[0], bob);

    EXPECT_TRUE(view.by_slug(ProfileFacet::Skill, "prof.cook").empty());
    EXPECT_TRUE(view.by_slug(ProfileFacet::Skill, "").empty());
}

// ── Lifecycle: only the owner retires their own fact (§8.6) ───────────────────

TEST_F(ProfileViewTest, OwnerClosesOwnNeedForeignLinkIgnored) {
    const UserId alice   = make_owner(0xA1);
    const UserId mallory = make_owner(0xEE);

    const Block need = add(alice, fact("Нужен электрик",
                                       {"kind:need", "cat:need.electrical"}));
    const Block food = add(alice, fact("Нужен повар",
                                       {"kind:need", "cat:need.food"}));

    // Mallory cannot retire Alice's need: the link lives in the wrong chain.
    records::ConceptLink forged;
    forged.from = ref_to(mallory, food);
    forged.to   = ref_to(alice,   food);
    forged.kind = "закрыто";
    add(mallory, forged);

    // Alice retires her own need.
    records::ConceptLink close;
    close.from = ref_to(alice, need);
    close.to   = ref_to(alice, need);
    close.kind = "закрыто";
    add(alice, close);

    const auto view    = ProfileView::build(*storage_);
    const auto profile = view.chain(alice);
    ASSERT_TRUE(profile.has_value());

    const auto needs = profile->by_facet(ProfileFacet::Need);
    ASSERT_EQ(needs.size(), 2u);
    for (const auto* n : needs) {
        if (n->slug == "need.electrical") { EXPECT_TRUE(n->closed); }
        if (n->slug == "need.food")       { EXPECT_FALSE(n->closed); }  // forgery ignored
    }

    // A closed fact drops out of matching; the forged one stays in.
    EXPECT_TRUE(view.by_slug(ProfileFacet::Need, "need.electrical").empty());
    EXPECT_EQ(view.by_slug(ProfileFacet::Need, "need.food").size(), 1u);
}

// ── Deterministic order: branch, then block ───────────────────────────────────

TEST_F(ProfileViewTest, FactsOrderedByBranchThenBlock) {
    const UserId alice = make_owner(0xA1);

    add(alice, fact("третий", {"kind:skill"}), 0x7FFF'FFFFu);
    add(alice, fact("первый", {"kind:skill"}), 0x10u);
    add(alice, fact("второй", {"kind:skill"}), 0x10u);

    const auto profile = ProfileView::build(*storage_).chain(alice);
    ASSERT_TRUE(profile.has_value());
    ASSERT_EQ(profile->facts.size(), 3u);
    EXPECT_EQ(profile->facts[0].text, "первый");
    EXPECT_EQ(profile->facts[1].text, "второй");
    EXPECT_EQ(profile->facts[2].text, "третий");
}

// ── Tag parsing helpers ───────────────────────────────────────────────────────

TEST_F(ProfileViewTest, FacetTagParsing) {
    EXPECT_EQ(ProfileView::facet_from_tag("kind:skill"), ProfileFacet::Skill);
    EXPECT_EQ(ProfileView::facet_from_tag("kind:need"),  ProfileFacet::Need);
    EXPECT_EQ(ProfileView::facet_from_tag("kind:obstacle"),
              ProfileFacet::Obstacle);
    EXPECT_FALSE(ProfileView::facet_from_tag("kind:nonsense").has_value());
    EXPECT_FALSE(ProfileView::facet_from_tag("cat:prof.cook").has_value());
    EXPECT_FALSE(ProfileView::facet_from_tag("skill").has_value());

    EXPECT_STREQ(ProfileView::facet_name(ProfileFacet::Industry), "industry");
}
