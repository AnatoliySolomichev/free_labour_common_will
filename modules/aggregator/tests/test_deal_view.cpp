#include "aggregator/deal_view.h"
#include "labor_thread_support.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>
#include <filesystem>

// Ось для разбивки цены: содержание здесь не проверяется, важно лишь что
// приёмки без разбивки не существует (records.md §9.5 v4).
inline records::BlockPath test_axis() {
    records::BlockPath r{};
    r.chain.fill(0x79);
    r.hash.fill(0x01);
    return r;
}

using namespace aggregator;
using namespace blockchain;

namespace {

UserId make_owner(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

records::Concept need_fact(const std::string& text) {
    records::Concept c;
    c.text = text;
    c.tags = {"kind:need", "cat:need.electrical", "urgency:high"};
    return c;
}

records::Concept skill_fact(const std::string& text) {
    records::Concept c;
    c.text = text;
    c.tags = {"kind:skill", "cat:prof.electrician"};
    return c;
}

records::Ref ref_of(const UserId& chain, const Hash& hash) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = hash.bytes;
    return r;
}

records::ConceptLink link(const records::Ref& from, const records::Ref& to,
                          const std::string& kind) {
    records::ConceptLink cl;
    cl.from = from;
    cl.to   = to;
    cl.kind = kind;
    return cl;
}

} // namespace

class DealViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    LaborThreads threads_;   // records.md §9.4 v4: works go into the thread

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_dealview_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    // Returns the block hash — deal links need it.
    Hash add(const UserId& owner, const records::Record& written) {
        const records::Record rec = threads_.stamp(owner.bytes, written);
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
        const Hash h = Crypto::hash_block(b);
        threads_.placed(owner.bytes, rec, h.bytes);
        return h;
    }
};

// ── The full circle, stage by stage ───────────────────────────────────────────

TEST_F(DealViewTest, StagesDeriveFromWhicheverLinksExist) {
    const UserId vera = make_owner(0xB2);   // seeker
    const UserId anna = make_owner(0xA1);   // worker

    // Open: a posted need and nothing else.
    const Hash need = add(vera, need_fact("Заменить проводку"));
    {
        const auto view = DealView::build(*storage_);
        ASSERT_EQ(view.deals().size(), 1u);
        EXPECT_EQ(view.deals()[0].stage(), DealStage::Open);
        EXPECT_EQ(view.deals()[0].urgency, "high");
    }

    // Taken: Anna volunteers with her own skill.
    const Hash skill = add(anna, skill_fact("Электромонтаж"));
    add(anna, link(ref_of(anna, skill), ref_of(vera, need), "берусь"));
    {
        const auto view = DealView::build(*storage_);
        const auto& d = view.deals()[0];
        EXPECT_EQ(d.stage(), DealStage::Taken);
        ASSERT_EQ(d.takers.size(), 1u);
        EXPECT_EQ(d.takers[0].chain, anna);
    }

    // Hired: Vera pledges 6h naming Anna.
    records::Pledge pledge;
    pledge.target    = ref_of(vera, need);
    pledge.units     = 6;
    pledge.executor  = anna.bytes;
    pledge.timestamp = 1000;
    const Hash pledge_hash = add(vera, pledge);
    {
        const auto view = DealView::build(*storage_);
        EXPECT_EQ(view.deals()[0].stage(), DealStage::Hired);
    }

    // Worked: Anna logs the work and attaches it with "исполняет".
    records::WorkRecord work;
    work.action   = "Замена проводки";
    work.start_ts = 1000;
    work.hours    = 6;
    work.customer = vera.bytes;   // по потребности Веры — её заказ (records.md §9.4)
    const Hash work_hash = add(anna, work);
    add(anna, link(ref_of(anna, work_hash), ref_of(vera, need), "исполняет"));
    {
        const auto view = DealView::build(*storage_);
        const auto& d = view.deals()[0];
        EXPECT_EQ(d.stage(), DealStage::Worked);
        ASSERT_EQ(d.works.size(), 1u);
        EXPECT_EQ(d.works[0].worker, anna);
        EXPECT_DOUBLE_EQ(d.works[0].hours, 6);
        EXPECT_FALSE(d.works[0].accepted);
    }

    // Paid: Vera accepts at 6 labor-h and pays in the same record, settling her
    // pledge (v4). There is no «accepted, unpaid» (records.md §9.5, ИР-023).
    records::Acceptance acc{};
    acc.work        = ref_of(anna, work_hash);
    acc.quality     = "пройдено";
    acc.hours_raw   = 6;
    acc.labor_units = 6;
    acc.axes        = {{test_axis(), 6}};
    records::Transfer pay{};
    pay.from      = vera.bytes;
    pay.to        = anna.bytes;
    pay.origins   = { {vera.bytes, 6.0} };
    pay.accept    = acc;
    pay.settles   = ref_of(vera, pledge_hash);
    pay.timestamp = 1000;
    const Hash acc_hash = add(vera, pay);
    {
        const auto view = DealView::build(*storage_);
        const auto& d = view.deals()[0];
        EXPECT_EQ(d.stage(), DealStage::Paid);
        EXPECT_DOUBLE_EQ(d.appraised(), 6);
        EXPECT_DOUBLE_EQ(d.paid(), 6);
        ASSERT_EQ(d.pledges.size(), 1u);
        EXPECT_DOUBLE_EQ(d.pledges[0].settled, 6);
        EXPECT_FALSE(d.pledges[0].active());
    }

    // Closed: Vera retires the need, pointing at the acceptance (records.md §8.6).
    add(vera, link(ref_of(vera, acc_hash), ref_of(vera, need), "закрыто"));
    {
        const auto view = DealView::build(*storage_);
        EXPECT_EQ(view.deals()[0].stage(), DealStage::Closed);
    }
}

// ── Graceful degradation ─────────────────────────────────────────────────────

// A pledge is optional: work → acceptance → payment with no pledge is a
// complete deal (case 2 of the fork analysis).
TEST_F(DealViewTest, DealWithoutPledgeReachesPaid) {
    const UserId vera = make_owner(0xB2);
    const UserId anna = make_owner(0xA1);

    const Hash need = add(vera, need_fact("Починить розетку"));
    records::WorkRecord work;
    work.action   = "починил";
    work.hours    = 1;
    work.customer = vera.bytes;   // заказ Веры (records.md §9.4)
    const Hash wh = add(anna, work);
    add(anna, link(ref_of(anna, wh), ref_of(vera, need), "исполняет"));

    records::Acceptance acc{};
    acc.work        = ref_of(anna, wh);
    acc.labor_units = 1;
    acc.axes        = {{test_axis(), 1}};
    records::Transfer pay{};
    pay.from    = vera.bytes;
    pay.to      = anna.bytes;
    pay.origins = { {vera.bytes, 1.0} };
    pay.accept  = acc;
    add(vera, pay);

    const auto view = DealView::build(*storage_);
    ASSERT_EQ(view.deals().size(), 1u);
    EXPECT_EQ(view.deals()[0].stage(), DealStage::Paid);
    EXPECT_TRUE(view.deals()[0].pledges.empty());
}

// Many-to-many (case 4): two work sessions under one need.
TEST_F(DealViewTest, SeveralWorksUnderOneNeed) {
    const UserId vera = make_owner(0xB2);
    const UserId anna = make_owner(0xA1);

    const Hash need = add(vera, need_fact("Полная замена проводки"));
    for (int session = 0; session < 3; ++session) {
        records::WorkRecord work;
        work.action = "заход " + std::to_string(session + 1);
        work.hours  = 2;
        const Hash wh = add(anna, work);
        add(anna, link(ref_of(anna, wh), ref_of(vera, need), "исполняет"));
    }

    const auto view = DealView::build(*storage_);
    ASSERT_EQ(view.deals().size(), 1u);
    EXPECT_EQ(view.deals()[0].works.size(), 3u);
    EXPECT_EQ(view.deals()[0].stage(), DealStage::Worked);
}

// A direct deal (nobody posted a need) is anchored by its acceptance.
TEST_F(DealViewTest, DirectDealAnchoredByAcceptance) {
    const UserId friend_ = make_owner(0xB2);
    const UserId anna    = make_owner(0xA1);

    records::WorkRecord work;
    work.action   = "проводка по дружбе, в мессенджере договорились";
    work.hours    = 3;
    work.customer = friend_.bytes;   // договорились — заказ (records.md §9.4)
    const Hash wh = add(anna, work);

    records::Acceptance acc{};
    acc.work        = ref_of(anna, wh);
    acc.labor_units = 3;
    acc.axes        = {{test_axis(), 3}};
    records::Transfer pay{};
    pay.from    = friend_.bytes;
    pay.to      = anna.bytes;
    pay.origins = { {friend_.bytes, 3.0} };
    pay.accept  = acc;
    add(friend_, pay);

    const auto view = DealView::build(*storage_);
    ASSERT_EQ(view.deals().size(), 1u);
    const auto& d = view.deals()[0];
    EXPECT_FALSE(d.need_hash.has_value());
    EXPECT_EQ(d.seeker, friend_);
    EXPECT_EQ(d.stage(), DealStage::Paid);
}

// ── Spoof guards ─────────────────────────────────────────────────────────────

// You volunteer with your OWN skill and attach your OWN work — links whose
// from-side lives in someone else's chain are ignored.
TEST_F(DealViewTest, ForeignFromSideLinksIgnored) {
    const UserId vera    = make_owner(0xB2);
    const UserId anna    = make_owner(0xA1);
    const UserId mallory = make_owner(0xEE);

    const Hash need  = add(vera, need_fact("Проводка"));
    const Hash skill = add(anna, skill_fact("Электромонтаж"));

    // Mallory "volunteers" with Anna's skill and pins Anna's work — both fake.
    add(mallory, link(ref_of(anna, skill), ref_of(vera, need), "берусь"));
    records::WorkRecord work;
    work.action = "чужая работа";
    work.hours  = 5;
    const Hash wh = add(anna, work);
    add(mallory, link(ref_of(anna, wh), ref_of(vera, need), "исполняет"));

    const auto view = DealView::build(*storage_);
    ASSERT_EQ(view.deals().size(), 1u);
    EXPECT_TRUE(view.deals()[0].takers.empty());
    EXPECT_TRUE(view.deals()[0].works.empty());
    EXPECT_EQ(view.deals()[0].stage(), DealStage::Open);
}

// ── for_chain: my deals, from any side ────────────────────────────────────────

TEST_F(DealViewTest, ForChainSeesSeekerAndWorkerSides) {
    const UserId vera  = make_owner(0xB2);
    const UserId anna  = make_owner(0xA1);
    const UserId other = make_owner(0xC3);

    const Hash need  = add(vera, need_fact("Проводка"));
    const Hash skill = add(anna, skill_fact("Электромонтаж"));
    add(anna, link(ref_of(anna, skill), ref_of(vera, need), "берусь"));
    add(other, need_fact("Чужая потребность, никем не тронутая"));

    const auto view = DealView::build(*storage_);
    ASSERT_EQ(view.deals().size(), 2u);

    EXPECT_EQ(view.for_chain(vera).size(),  1u);   // her need
    EXPECT_EQ(view.for_chain(anna).size(),  1u);   // she volunteered
    EXPECT_EQ(view.for_chain(other).size(), 1u);   // only their own
}

// Засчитывается сделка того, кому продавец отдал результат (records.md §9.5,
// ИР-023): чужая оплата той же работы ничтожна — она не затирает сделку и не
// делает работу «проданной дважды», только видна в счётчике. Двойную продажу
// может устроить лишь сам продавец — отпустив результат второму.
TEST_F(DealViewTest, AStrangersPaymentIsVoidAndOnlyTheSellerSellsTwice) {
    const UserId anna  = make_owner(0xA1);
    const UserId vera  = make_owner(0xB2);
    const UserId boris = make_owner(0xC3);

    records::WorkRecord w;
    w.action   = "Проводка";
    w.hours    = 3;
    w.customer = vera.bytes;                                  // заказ Веры
    const Hash wh = add(anna, w);
    const auto deal = [&](const UserId& by,
                          std::optional<records::Ref> release = std::nullopt) {
        records::Acceptance a{};
        a.work        = ref_of(anna, wh);
        a.hours_raw   = 3;
        a.labor_units = 3;
        a.axes        = {{test_axis(), 3.0}};
        a.release     = release;
        records::Transfer t{};
        t.from      = by.bytes;
        t.to        = anna.bytes;
        t.origins   = {{by.bytes, 3.0}};
        t.accept    = a;
        t.timestamp = 1000;
        return add(by, t);
    };
    const Hash by_vera = deal(vera);
    {
        const auto view = DealView::build(*storage_);
        ASSERT_EQ(view.deals().size(), 1u);
        const auto& dw = view.deals()[0].works.at(0);
        EXPECT_EQ(dw.acceptance_hash.bytes, by_vera.bytes);
        EXPECT_EQ(dw.acceptances, 1u);
        EXPECT_FALSE(dw.oversold);
        ASSERT_TRUE(dw.customer.has_value());
        EXPECT_EQ(*dw.customer, vera);
        EXPECT_DOUBLE_EQ(dw.paid, 3.0);                        // оплачено той же записью
    }

    deal(boris);                                              // Борис платит без адреса
    {
        const auto view = DealView::build(*storage_);
        ASSERT_EQ(view.deals().size(), 1u);
        const auto& dw = view.deals()[0].works.at(0);
        EXPECT_EQ(dw.acceptance_hash.bytes, by_vera.bytes);   // сделка Веры стоит
        EXPECT_EQ(dw.acceptances, 1u);
        EXPECT_EQ(dw.unaddressed, 1u);                        // ничтожная — только в счётчике
        EXPECT_FALSE(dw.oversold);
    }

    records::Release rl{};                                    // Анна сама отдаёт работу Борису
    rl.work      = ref_of(anna, wh);
    rl.to        = boris.bytes;
    rl.timestamp = 900;
    rl.expires   = 2000;
    deal(boris, ref_of(anna, add(anna, rl)));
    const auto view = DealView::build(*storage_);
    ASSERT_EQ(view.deals().size(), 1u);
    const auto& dw = view.deals()[0].works.at(0);
    EXPECT_EQ(dw.acceptances, 2u);                            // обе — по адресу продавца
    EXPECT_TRUE(dw.oversold);                                 // продана дважды — ею самой
}
