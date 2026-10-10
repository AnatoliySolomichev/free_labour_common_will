// Кому сколько ресурса вещи принадлежит (ИР-013): доля — купленная порция
// ресурса, а не дробь. Витрина читает это из обычных записей: продажа
// машино-часов, покупка, расход купленного в свои работы.

#include "aggregator/resource_view.h"
#include "aggregator/aggregator.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/goods_thread.h>

#include <gtest/gtest.h>
#include <filesystem>

using namespace aggregator;
using namespace blockchain;

namespace {

constexpr int64_t kDay = 86'400;

UserId uid(uint8_t fill) { UserId u; u.bytes.fill(fill); return u; }

Block make_block(const UserId& owner, BlockIndex index, const records::Record& rec,
                 int64_t ts) {
    Block b{};
    b.address           = {owner, 0x7FFF'FFFFu, index};
    b.prev_hash         = Hash::zero();
    b.timestamp_claimed = ts;
    b.type              = BlockType::DATA;
    b.payload           = records::Codec::encode(rec);
    b.signature         = Signature::null();
    const auto bytes    = Serializer::encode(b);
    const KeyPair kp    = Crypto::generate_keypair();
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

records::Ref ref_to(const UserId& chain, const Block& b) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = Crypto::hash_block(b).bytes;
    return r;
}

} // namespace

class ResourceViewTest : public ::testing::Test {
protected:
    std::filesystem::path                db_;
    std::unique_ptr<AggregatorStorage>   storage_;
    UserId anna_ = uid(0xA1), boris_ = uid(0xB2), vera_ = uid(0xC3);
    BlockIndex next_ = 0;

    void SetUp() override {
        static int cnt = 0;
        db_ = std::filesystem::temp_directory_path() /
              ("agg_resource_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_);
        storage_ = std::make_unique<AggregatorStorage>(db_);
    }
    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_);
    }

    Block add(const UserId& owner, const records::Record& rec, int64_t ts) {
        const Block b = make_block(owner, next_++, rec, ts);
        EXPECT_TRUE(storage_->add_block(b));
        return b;
    }

    // Печь Анны: 300ч, ресурс 8000 моточасов.
    Block oven() {
        records::Tool t{};
        t.name  = "печь";
        t.cost  = 300.0;
        t.life  = 8000.0;
        t.basis = "est";
        t.note  = "оценка владельца";
        return add(anna_, t, kDay);
    }

    // Анна продаёт `hours` моточасов печи цепи `to`: запись без труда с одним
    // переносом (records.md §9.4 v8), отпуск на покупателя, его оплата.
    // Возвращает {работа-продажа, сделка}.
    std::pair<Block, Block> sell_hours(const Block& tool, const UserId& to,
                                       double hours, double collected_before,
                                       uint64_t seq, int64_t at,
                                       bool pay = true) {
        const double carried = hours / 8000.0 * 300.0;
        records::WorkRecord hire{};
        hire.action  = "машино-часы";
        hire.hours   = 0.0;
        hire.start_ts = at;
        hire.outputs = {{"машино-часы", hours, "маш·ч"}};
        records::CarryEntry ce{};
        ce.src     = ref_to(anna_, tool);
        ce.used    = hours;
        ce.carried = carried;
        ce.seq     = seq;
        ce.after   = collected_before + carried;
        hire.carry.push_back(ce);
        const Block hw = add(anna_, hire, at);

        records::Release rl{};
        rl.work      = ref_to(anna_, hw);
        rl.to        = to.bytes;
        rl.timestamp = at;
        rl.expires   = at + 10'000;
        const Block rb = add(anna_, rl, at);
        if (!pay) return {hw, rb};

        records::Acceptance a{};
        a.work          = ref_to(anna_, hw);
        a.release       = ref_to(anna_, rb);
        a.hours_raw     = 0.0;
        a.labor_units   = 0.0;
        a.carried_units = carried;
        records::Transfer t{};
        t.from      = to.bytes;
        t.to        = anna_.bytes;
        t.origins   = { {to.bytes, carried} };
        t.accept    = a;
        t.timestamp = at + 100;
        return {hw, add(to, t, at + 100)};
    }

    // Покупатель заводит купленные часы материалом своей покупки и тратит
    // `spent` из них в свою работу (records.md §10.1 v3).
    void spend_hours(const UserId& who, const Block& deal, double hours,
                     double paid, double spent, int64_t at) {
        records::Material m{};
        m.name  = "машино-часы печи";
        m.unit  = "маш·ч";
        m.cost  = paid;
        m.qty   = hours;
        m.basis = "paid";
        m.src   = ref_to(who, deal);            // своя покупка: перевод писал он сам
        records::LotLink lot{};      // звено подниточки своей покупки
        lot.out   = 0;
        lot.qty   = hours;
        lot.seq   = 0;
        lot.after = hours;
        m.lot     = lot;
        const Block mb = add(who, m, at);

        records::WorkRecord w{};
        w.action   = "выпечка";
        w.hours    = 4.0;
        w.start_ts = at;
        records::CarryEntry ce{};
        ce.src     = ref_to(who, mb);
        ce.used    = spent;
        ce.carried = spent / hours * paid;
        ce.seq     = 0;
        ce.after   = ce.carried;
        w.carry.push_back(ce);
        add(who, w, at + 20'000);
    }
};

TEST_F(ResourceViewTest, ResourceSoldForwardIsSeenAsShares) {
    const Block tool = oven();
    // Борис и Вера покупают по 2000 моточасов — по 75ч каждому.
    const auto b = sell_hours(tool, boris_, 2000.0, 0.0,  0, kDay * 2);
    const auto v = sell_hours(tool, vera_,  2000.0, 75.0, 1, kDay * 3);
    // Борис уже истратил 800 своих часов.
    spend_hours(boris_, b.second, 2000.0, 75.0, 800.0, kDay * 4);

    const auto split = ResourceView::build(*storage_, anna_,
                                           Crypto::hash_block(tool).bytes, kDay * 5);
    ASSERT_TRUE(split.known);
    EXPECT_EQ(split.keeper, anna_);
    EXPECT_EQ(split.name, "печь");
    EXPECT_NEAR(split.life, 8000.0, 1e-9);
    EXPECT_NEAR(split.sold_hours, 4000.0, 1e-9);
    EXPECT_NEAR(split.free_hours, 4000.0, 1e-9);     // половина ресурса ещё у Анны
    EXPECT_NEAR(split.collected, 150.0, 1e-9);       // возмещено ровно проданным
    EXPECT_NEAR(split.residual, 150.0, 1e-9);
    EXPECT_FALSE(split.over_used);

    ASSERT_EQ(split.holders.size(), 2u);
    const auto& first = split.holders[0];
    EXPECT_NEAR(first.hours, 2000.0, 1e-9);
    EXPECT_NEAR(first.paid,  75.0, 1e-9);
    const auto boris_it = std::find_if(split.holders.begin(), split.holders.end(),
        [&](const ResourceHolder& h) { return h.chain == boris_; });
    ASSERT_NE(boris_it, split.holders.end());
    EXPECT_NEAR(boris_it->spent, 800.0, 1e-9);       // истратил
    EXPECT_NEAR(boris_it->left, 1200.0, 1e-9);       // осталось
    const auto vera_it = std::find_if(split.holders.begin(), split.holders.end(),
        [&](const ResourceHolder& h) { return h.chain == vera_; });
    ASSERT_NE(vera_it, split.holders.end());
    EXPECT_NEAR(vera_it->spent, 0.0, 1e-9);
    EXPECT_NEAR(vera_it->left, 2000.0, 1e-9);
}

// Держатель и сам тратит ресурс в своей продукции — это не продажа, а расход.
TEST_F(ResourceViewTest, KeepersOwnUseIsCountedApartFromSales) {
    const Block tool = oven();
    sell_hours(tool, boris_, 2000.0, 0.0, 0, kDay * 2);

    records::WorkRecord bake{};
    bake.action   = "выпечка хлеба";
    bake.hours    = 6.0;
    bake.start_ts = kDay * 4;
    records::CarryEntry ce{};
    ce.src     = ref_to(anna_, tool);
    ce.used    = 1000.0;
    ce.carried = 1000.0 / 8000.0 * 300.0;
    ce.seq     = 1;
    ce.after   = 75.0 + ce.carried;
    bake.carry.push_back(ce);
    add(anna_, bake, kDay * 4 + 30'000);

    const auto split = ResourceView::build(*storage_, anna_,
                                           Crypto::hash_block(tool).bytes, kDay * 5);
    EXPECT_NEAR(split.sold_hours, 2000.0, 1e-9);
    EXPECT_NEAR(split.keeper_used, 1000.0, 1e-9);
    EXPECT_NEAR(split.free_hours, 5000.0, 1e-9);
    ASSERT_EQ(split.holders.size(), 1u);
}

// Отпущенное и не оплаченное — ещё не чья-то доля, но ресурс уже обещан.
TEST_F(ResourceViewTest, ReleasedButUnpaidHoursAreShownApart) {
    const Block tool = oven();
    sell_hours(tool, boris_, 1000.0, 0.0, 0, kDay * 2, /*pay=*/false);

    const auto split = ResourceView::build(*storage_, anna_,
                                           Crypto::hash_block(tool).bytes, kDay * 3);
    EXPECT_NEAR(split.sold_hours, 0.0, 1e-9);
    EXPECT_NEAR(split.pending_hours, 1000.0, 1e-9);
    EXPECT_NEAR(split.free_hours, 7000.0, 1e-9);
    EXPECT_TRUE(split.holders.empty());
}

// Распорядиться большим ресурсом, чем есть, протокол не помешает — но это
// видно числом: держатель работает на том, что уже продано.
TEST_F(ResourceViewTest, DisposingMoreThanTheResourceIsVisible) {
    const Block tool = oven();
    sell_hours(tool, boris_, 5000.0, 0.0,   0, kDay * 2);
    sell_hours(tool, vera_,  4000.0, 187.5, 1, kDay * 3);

    const auto split = ResourceView::build(*storage_, anna_,
                                           Crypto::hash_block(tool).bytes, kDay * 4);
    EXPECT_NEAR(split.sold_hours, 9000.0, 1e-9);     // при ресурсе 8000
    EXPECT_TRUE(split.over_used);
    EXPECT_NEAR(split.free_hours, 0.0, 1e-9);
}

TEST_F(ResourceViewTest, UnknownToolIsReportedAsUnknown) {
    std::array<uint8_t, 32> nothing{};
    nothing.fill(0x77);
    EXPECT_FALSE(ResourceView::build(*storage_, anna_, nothing, kDay).known);
}

// ── Согласия на запись (ИР-012) ──────────────────────────────────────────────
//
// Общего объекта «решение артели» не образуется: порог считается по N
// подписанным записям. Проверяем именно это — и что позднее слово цепи
// отменяет прежнее.

TEST_F(ResourceViewTest, ConsentsAreGatheredPerRecord) {
    const Block draft = oven();                 // черновик в цепи Анны
    const auto target = Crypto::hash_block(draft).bytes;

    records::Consent yes{};
    yes.target = ref_to(anna_, draft);
    yes.agreed = true;
    yes.note   = "согласна";
    add(boris_, yes, kDay * 2);

    records::Consent no{};
    no.target = ref_to(anna_, draft);
    no.agreed = false;
    add(vera_, no, kDay * 2);

    const auto deals = read_deals(*storage_);
    const auto& said = deals.consents.at(target);
    EXPECT_EQ(said.size(), 2u);
    EXPECT_TRUE(said.at(boris_));
    EXPECT_FALSE(said.at(vera_));

    const auto agreed = deals.consented(target);
    EXPECT_EQ(agreed.size(), 1u);               // против — не согласие
    EXPECT_TRUE(agreed.count(boris_) > 0);
}

TEST_F(ResourceViewTest, LaterWordOfTheSameChainWins) {
    const Block draft = oven();
    const auto target = Crypto::hash_block(draft).bytes;

    records::Consent first{};
    first.target = ref_to(anna_, draft);
    first.agreed = true;
    add(boris_, first, kDay * 2);

    records::Consent second{};
    second.target = ref_to(anna_, draft);
    second.agreed = false;                      // передумал
    add(boris_, second, kDay * 3);

    const auto deals = read_deals(*storage_);
    EXPECT_FALSE(deals.consents.at(target).at(boris_));
    EXPECT_TRUE(deals.consented(target).empty());
}

// Себя не спрашивают: подпись под своей же записью и есть согласие.
TEST_F(ResourceViewTest, SelfConsentIsIgnored) {
    const Block draft = oven();
    records::Consent mine{};
    mine.target = ref_to(anna_, draft);
    mine.agreed = true;
    add(anna_, mine, kDay * 2);

    const auto deals = read_deals(*storage_);
    EXPECT_EQ(deals.consents.count(Crypto::hash_block(draft).bytes), 0u);
}
