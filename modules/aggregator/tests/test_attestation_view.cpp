#include "aggregator/attestation_view.h"

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

records::Ref ref_of(const UserId& chain, const Hash& hash) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = hash.bytes;
    return r;
}

} // namespace

class AttestationViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_attview_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    Hash add(const UserId& owner, const records::Record& rec) {
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
        return Crypto::hash_block(b);
    }

    // Worker's economic foundation: Specialty (slug) + Grade, own chain.
    std::pair<Hash, Hash> foundation(const UserId& worker,
                                     const std::string& slug, uint8_t level) {
        const Hash spec = add(worker, records::Specialty{slug});
        records::Grade g;
        g.specialty = ref_of(worker, spec);
        g.level     = level;
        return {spec, add(worker, g)};
    }

    // One accepted job: worker logs `hours` under `grade`, acceptor accepts.
    void accepted_work(const UserId& worker, const Hash& grade,
                       const UserId& acceptor, double hours) {
        records::WorkRecord w;
        w.agent  = ref_of(worker, grade);
        w.action = "работа";
        w.hours  = hours;
        const Hash wh = add(worker, w);
        records::Acceptance a;
        a.work        = ref_of(worker, wh);
        a.receiver    = acceptor.bytes;
        a.hours_raw   = hours;
        a.labor_units = hours;
        add(acceptor, a);
    }
};

// Distinct chains are counted, never acceptances; the worker's own chain never
// attests itself — that vouch is free.
TEST_F(AttestationViewTest, DistinctChainsAndNoSelfVouch) {
    const UserId anna  = make_owner(0xA1);
    const UserId vera  = make_owner(0xB2);
    const UserId boris = make_owner(0xC3);

    const auto [spec, grade] = foundation(anna, "prof.electrician", 4);
    accepted_work(anna, grade, vera,  6);
    accepted_work(anna, grade, vera,  4);   // same chain again — one witness
    accepted_work(anna, grade, boris, 2);
    accepted_work(anna, grade, anna,  99);  // self-acceptance — ignored

    const auto view = AttestationView::build(*storage_);
    const auto att  = view.at_level(anna, "prof.electrician", 4);
    ASSERT_TRUE(att.has_value());
    EXPECT_EQ(att->chains, 2u);             // Vera + Boris, not 3 acceptances
    EXPECT_DOUBLE_EQ(att->hours, 12.0);     // 6+4+2, self excluded

    EXPECT_FALSE(view.at_level(anna, "prof.electrician", 5).has_value());
    EXPECT_FALSE(view.at_level(vera, "prof.electrician", 4).has_value());
}

// Every hop must stay in the worker's own chain: a Grade or Specialty living
// elsewhere attests nothing.
TEST_F(AttestationViewTest, ForeignGradeOrSpecialtyIgnored) {
    const UserId anna    = make_owner(0xA1);
    const UserId vera    = make_owner(0xB2);
    const UserId mallory = make_owner(0xEE);

    // Mallory logs work pointing at ANNA's grade — not Mallory's own.
    const auto [spec, anna_grade] = foundation(anna, "prof.electrician", 6);
    records::WorkRecord w;
    w.agent  = ref_of(anna, anna_grade);
    w.action = "чужой разряд";
    w.hours  = 5;
    const Hash wh = add(mallory, w);
    records::Acceptance a;
    a.work      = ref_of(mallory, wh);
    a.receiver  = vera.bytes;
    a.hours_raw = 5;
    add(vera, a);

    const auto view = AttestationView::build(*storage_);
    EXPECT_FALSE(view.at_level(mallory, "prof.electrician", 6).has_value());
    EXPECT_FALSE(view.at_level(anna,    "prof.electrician", 6).has_value());
}

// best() picks the highest attested level — the reader compares it with the
// claim and decides; nothing is scored globally.
TEST_F(AttestationViewTest, BestPicksHighestAttestedLevel) {
    const UserId anna = make_owner(0xA1);
    const UserId vera = make_owner(0xB2);

    const auto [s3, g3] = foundation(anna, "prof.electrician", 3);
    const auto [s4, g4] = foundation(anna, "prof.electrician", 4);
    accepted_work(anna, g3, vera, 100);
    accepted_work(anna, g4, vera, 8);

    const auto view = AttestationView::build(*storage_);
    const auto best = view.best(anna, "prof.electrician");
    ASSERT_TRUE(best.has_value());
    EXPECT_EQ(best->level, 4);
    EXPECT_DOUBLE_EQ(best->hours, 8.0);

    EXPECT_FALSE(view.best(anna, "prof.cook").has_value());
}
