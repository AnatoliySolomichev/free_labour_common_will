// The deals on the table as offers (economy.md §4.4, records.md §11.13): hours
// move only when the seller accepts by the term; paper someone else issued is
// spent from bills, and paper nobody had moves nothing — and is seen.

#include "aggregator/footprint_view.h"
#include "aggregator/recognition.h"
#include "labor_thread_support.h"
#include "settlement_support.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <gtest/gtest.h>

#include <ctime>
#include <filesystem>

using namespace aggregator;
using namespace blockchain;

namespace {

constexpr uint32_t kLeaf  = 0x7FFF'FFFFu;
constexpr uint32_t kSafe  = 0x7FFF'FFFEu;   // a second branch: the savings purse
// Deals come after the works they pay for, and the fixtures date works from
// 2001 on (labor_thread_support.h): every moment of a deal counts from here.
constexpr int64_t  kT0    = 2'000'000'000;
constexpr int64_t  kNow   = kT0 + 1'000'000;

UserId make_chain(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

records::BlockPath test_axis() {
    records::BlockPath r{};
    r.chain.fill(0x79);
    r.hash.fill(0x01);
    return r;
}

} // namespace

class SettlementTableTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    LaborThreads                       threads_;
    const UserId anna_  = make_chain(0xA1);   // продаёт
    const UserId boris_ = make_chain(0xB2);   // покупает
    const UserId vera_  = make_chain(0xC3);   // чья бумага ходит
    const UserId petr_  = make_chain(0xD4);   // ещё один продавец
    const UserId gleb_  = make_chain(0xE5);   // продавец со сплошным кошельком

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_settlement_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }
    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    // One block exactly as written, in branch `node`.
    Block put(const UserId& owner, const records::Record& written, uint32_t node = kLeaf) {
        return put_at(owner, written, node, next_index_++);
    }

    // The same at position `index` of the branch — for a purse written block by
    // block, with nothing missing.
    Block put_at(const UserId& owner, const records::Record& written, uint32_t node,
                 BlockIndex index) {
        const records::Record rec = threads_.stamp(owner.bytes, written);
        Block b{};
        b.address           = {owner, node, index};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 1000;
        if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            const auto end = static_cast<Timestamp>(records::labor_interval(*w).second);
            if (end > b.timestamp_claimed) b.timestamp_claimed = end;
        }
        b.type      = BlockType::DATA;
        b.payload   = records::Codec::encode(rec);
        b.signature = Signature::null();
        const auto    bytes = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        EXPECT_TRUE(storage_->add_block(b));
        threads_.placed(owner.bytes, rec, Crypto::hash_block(b).bytes);
        return b;
    }

    void sign_and_add(Block& b) {
        b.signature         = Signature::null();
        const auto    bytes = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        EXPECT_TRUE(storage_->add_block(b));
    }

    // A block with nothing in it (blockchain.md §5.4): it only says «the branch
    // was here by then».
    Block stub(const UserId& owner, BlockIndex index, Timestamp when, uint32_t node = kLeaf) {
        Block b{};
        b.address           = {owner, node, index};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = when;
        b.type              = BlockType::DATA;
        b.payload           = {0x80};
        sign_and_add(b);
        return b;
    }

    // `owner`'s half of a merge (blockchain.md §6.4): the block at `index` of its
    // branch, taking the partner's last block `took`, co-signed. Nobody checks
    // the co-signature on this table (sync.md §10.10).
    Block merge(const UserId& owner, BlockIndex index, const Block& took, Timestamp when,
                uint32_t node = kLeaf) {
        MergePayload mp{};
        mp.partner_last_address = took.address;
        mp.partner_last_hash    = Crypto::hash_block(took);
        mp.merge_timestamp      = when;
        Block b{};
        b.address           = {owner, node, index};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = when;
        b.type              = BlockType::MERGE;
        b.payload           = Serializer::encode(mp);
        b.co_signature      = Signature::null();
        sign_and_add(b);
        return b;
    }

    static records::Ref ref(const UserId& chain, const Block& b) {
        return records::Ref{chain.bytes, Crypto::hash_block(b).bytes};
    }

    // A job ordered by `customer` — addressed to him in advance.
    static records::WorkRecord work_for(const UserId& customer, double hours) {
        records::WorkRecord w{};
        w.action   = "работа";
        w.hours    = hours;
        w.customer = customer.bytes;
        return w;
    }
    Block job(const UserId& worker, const UserId& customer, double hours) {
        return put(worker, work_for(customer, hours));
    }

    // `payer` accepts `work` of `worker` and pays with `origins`, answer by `until`.
    records::Transfer offer(const UserId& payer, const UserId& worker, const Block& work,
                            std::vector<records::OriginQty> origins, int64_t ts = kT0 + 100,
                            int64_t until = kNow) {
        double price = 0;
        for (const auto& o : origins) price += o.units;
        records::Acceptance a{};
        a.work        = ref(worker, work);
        a.hours_raw   = price;
        a.labor_units = price;
        a.axes        = {{test_axis(), price}};
        records::Transfer t{};
        t.from      = payer.bytes;
        t.to        = worker.bytes;
        t.to_node   = kLeaf;
        t.origins   = std::move(origins);
        t.accept    = a;
        t.timestamp = ts;
        t.until     = until;
        return t;
    }

    static records::DealReply reply(const Block& deal, const UserId& payer, bool accepted,
                                    int64_t ts) {
        records::DealReply r{};
        r.deal      = ref(payer, deal);
        r.accepted  = accepted;
        r.note      = accepted ? "" : "не беру";
        r.timestamp = ts;
        return r;
    }
    Block answer(const UserId& seller, const Block& deal, const UserId& payer, bool accepted,
                 int64_t ts = kT0 + 150, uint32_t node = kLeaf) {
        return put(seller, reply(deal, payer, accepted, ts), node);
    }

    // Anna sells Boris a tent on a pre-signed return to herself by kT0 + 9000;
    // Boris pays with 4h of Vera's paper. With `deposit` the payment is the
    // deposit (records.md §9.8 v9). Returns the purchase.
    Block tent_on_return(bool deposit) {
        const Block src = vera_pays(boris_, 10.0);
        records::WorkRecord make{};
        make.hours   = 4.0;
        make.outputs = {{"палатка", 1.0, "шт"}};
        make.axes    = {{test_axis(), 1.0}};
        tent_ = put(anna_, make);
        records::Release rl{};
        rl.work      = ref(anna_, tent_);
        rl.to        = boris_.bytes;
        rl.timestamp = kT0 + 50;
        rl.expires   = kT0 + 5000;
        rl.addressed = records::Addressed{anna_.bytes, kT0 + 9000};
        const Block rb = put(anna_, rl);
        records::Transfer buy = offer(boris_, anna_, tent_, {bill(vera_, 4.0, ref(vera_, src))});
        buy.accept->release = ref(anna_, rb);
        buy.accept->buyback = records::Buyback{anna_.bytes, kT0 + 9000, false, deposit};
        const Block held = put(boris_, buy);
        answer(anna_, held, boris_, true);
        return held;
    }
    Block tent_;

    // Anna buys the tent back, paying with `origins`.
    Block buy_back(const Block& held, std::vector<records::OriginQty> origins) {
        records::Acceptance ba{};
        ba.work          = ref(anna_, tent_);
        ba.release       = ref(boris_, held);
        ba.carried_units = 4.0;
        records::Transfer back{};
        back.from      = anna_.bytes;
        back.to        = boris_.bytes;
        back.to_node   = kLeaf;
        back.origins   = std::move(origins);
        back.accept    = ba;
        back.timestamp = kT0 + 600;
        back.until     = kNow;
        return put(anna_, back);
    }

    // Vera pays `holder` 10h of her own paper for a job; the holder accepts.
    // Returns the deal — the bill the holder now spends from.
    Block vera_pays(const UserId& holder, double units = 10.0) {
        const Block w = job(holder, vera_, units);
        const Block d = put(vera_, offer(vera_, holder, w, {{vera_.bytes, units}}));
        answer(holder, d, vera_, true);
        return d;
    }

    static records::OriginQty bill(const UserId& issuer, double units, const records::Ref& src,
                                   std::optional<records::Ref> prev = std::nullopt) {
        return records::OriginQty{issuer.bytes, units, src, prev};
    }

    static std::array<uint8_t, 32> h(const Block& b) { return Crypto::hash_block(b).bytes; }

    // The table lists deals in block-hash order: find one by its block.
    static const DealEntry& entry(const DealsOnTable& deals, const Block& b) {
        for (const auto& d : deals.all)
            if (d.hash == h(b)) return d;
        throw std::runtime_error("deal not on the table");
    }
};

// ── Ответ продавца ───────────────────────────────────────────────────────────

// Оферта до ответа ничего не двигает — и становится сделкой, как только продавец
// принял её в срок.
TEST_F(SettlementTableTest, AnOfferMovesNothingUntilTheSellerAccepts) {
    const Block w = job(anna_, boris_, 3.0);
    const Block d = put(boris_, offer(boris_, anna_, w, {{boris_.bytes, 3.0}}));
    {
        const auto deals = read_deals(*storage_);
        EXPECT_FALSE(deals.moves_hours(h(d)));
        EXPECT_TRUE(deals.recognition.unsettled.count(h(d)));
        EXPECT_STREQ(deal_standing(deals, entry(deals, d), kT0 + 500), "pending");
        EXPECT_STREQ(deal_standing(deals, entry(deals, d), kNow + 1), "expired");
    }
    answer(anna_, d, boris_, true);
    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.counts(h(d)));
    EXPECT_EQ(deals.answer(h(d)), records::Answer::accepted);
    ASSERT_NE(deals.counted_reply(h(d)), nullptr);
}

// Отказ — сделки не было; новая оферта того же покупателя — не «повтор».
TEST_F(SettlementTableTest, ARefusalVoidsTheOfferAndTheNextOneCounts) {
    const Block w  = job(anna_, boris_, 3.0);
    const Block d1 = put(boris_, offer(boris_, anna_, w, {{boris_.bytes, 3.0}}));
    answer(anna_, d1, boris_, false);
    const Block d2 = put(boris_, offer(boris_, anna_, w, {{boris_.bytes, 3.0}}, kT0 + 200));
    answer(anna_, d2, boris_, true, kT0 + 250);
    const auto deals = read_deals(*storage_);
    EXPECT_FALSE(deals.moves_hours(h(d1)));
    EXPECT_STREQ(deal_standing(deals, entry(deals, d1), kT0 + 300), "refused");
    EXPECT_TRUE(deals.counts(h(d2)));
    EXPECT_TRUE(deals.recognition.repeated.empty());
}

// Ответ после срока ничего не отвечает; отвечает только кошелёк, в который платят.
TEST_F(SettlementTableTest, OnlyThePurseBeingPaidAnswersAndOnlyInTime) {
    const Block w  = job(anna_, boris_, 3.0);
    const Block d1 = put(boris_, offer(boris_, anna_, w, {{boris_.bytes, 3.0}}, kT0 + 100, kT0 + 200));
    answer(anna_, d1, boris_, true, kT0 + 201);                       // поздно
    const Block w2 = job(anna_, boris_, 2.0);
    const Block d2 = put(boris_, offer(boris_, anna_, w2, {{boris_.bytes, 2.0}}));
    answer(petr_, d2, boris_, true);                            // посторонний
    answer(anna_, d2, boris_, true, kT0 + 150, kSafe);                // не тот кошелёк
    const auto deals = read_deals(*storage_);
    EXPECT_FALSE(deals.moves_hours(h(d1)));
    EXPECT_FALSE(deals.moves_hours(h(d2)));
    EXPECT_EQ(deals.answer(h(d2)), records::Answer::none);
}

// ── Векселя ──────────────────────────────────────────────────────────────────

// Бумага, которой у плательщика не было, — не бумага: сделка ничего не двигает,
// у эмитента не появляется лишней бумаги в обороте, а попытка видна в следе
// плательщика.
TEST_F(SettlementTableTest, PaperThePayerNeverHadMovesNothingAndIsSeen) {
    const Block w = job(anna_, boris_, 5.0);
    records::Ref nowhere{boris_.bytes, {}};
    nowhere.hash.fill(0xEE);
    const Block d = put(boris_, offer(boris_, anna_, w, {bill(vera_, 5.0, nowhere)}));
    answer(anna_, d, boris_, true);                             // продавец принял, не проверив
    const auto deals = read_deals(*storage_);
    EXPECT_FALSE(deals.moves_hours(h(d)));
    EXPECT_TRUE(deals.unbacked.count(h(d)));
    EXPECT_STREQ(deal_standing(deals, entry(deals, d), kT0 + 500), "unbacked");

    const auto view = FootprintView::build(*storage_);
    const auto b = view.chain(boris_);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->unbacked_deals, 1u);
    EXPECT_DOUBLE_EQ(b->unbacked_units, 5.0);
    const auto v = view.chain(vera_);
    EXPECT_FALSE(v.has_value() && v->paper_outstanding > 0);   // бумаги Веры не прибавилось
}

// Вексель тратится по нити: сверх остатка — не бумага, и всё после разрыва тоже.
TEST_F(SettlementTableTest, ABillCannotBeSpentTwice) {
    const Block src = vera_pays(boris_, 10.0);
    const Block w1  = job(anna_, boris_, 6.0);
    const Block d1  = put(boris_, offer(boris_, anna_, w1, {bill(vera_, 6.0, ref(vera_, src))}));
    answer(anna_, d1, boris_, true);
    const Block w2  = job(petr_, boris_, 6.0);
    const Block d2  = put(boris_, offer(boris_, petr_, w2,
                                        {bill(vera_, 6.0, ref(vera_, src), ref(boris_, d1))}));
    answer(petr_, d2, boris_, true);
    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.counts(h(d1)));
    EXPECT_FALSE(deals.moves_hours(h(d2)));                     // 6 + 6 > 10
    EXPECT_EQ(deals.unbacked_why.at(h(d2)), "трата больше остатка векселя");
    const auto it = deals.bills.find(BillKey{boris_.bytes, vera_.bytes, h(src), vera_.bytes});
    ASSERT_NE(it, deals.bills.end());
    EXPECT_DOUBLE_EQ(it->second.state.remaining(), 4.0);
}

// Скрытая трата рвёт нить: вторая трата обязана назвать первую.
TEST_F(SettlementTableTest, AHiddenSpendBreaksTheThread) {
    const Block src = vera_pays(boris_, 10.0);
    const Block w1  = job(anna_, boris_, 3.0);
    const Block d1  = put(boris_, offer(boris_, anna_, w1, {bill(vera_, 3.0, ref(vera_, src))}));
    answer(anna_, d1, boris_, true);
    const Block w2  = job(petr_, boris_, 3.0);
    const Block d2  = put(boris_, offer(boris_, petr_, w2, {bill(vera_, 3.0, ref(vera_, src))}));
    answer(petr_, d2, boris_, true);
    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.counts(h(d1)));
    EXPECT_FALSE(deals.moves_hours(h(d2)));
}

// Отвергнутая или просроченная оферта отпускает часы векселя: заплатить ими снова
// можно — после отказа сразу, после молчания — когда вышел срок.
TEST_F(SettlementTableTest, AnOfferThatDiedFreesTheBill) {
    const Block src = vera_pays(boris_, 10.0);
    const Block w1  = job(anna_, boris_, 10.0);
    const Block d1  = put(boris_, offer(boris_, anna_, w1, {bill(vera_, 10.0, ref(vera_, src))},
                                        kT0 + 100, kT0 + 300));
    // Анна молчит. До срока — повторная трата сверх остатка; после — законна.
    const Block w2  = job(petr_, boris_, 10.0);
    const Block d2  = put(boris_, offer(boris_, petr_, w2,
                                        {bill(vera_, 10.0, ref(vera_, src), ref(boris_, d1))},
                                        kT0 + 400));
    answer(petr_, d2, boris_, true, kT0 + 450);
    const auto deals = read_deals(*storage_);
    EXPECT_FALSE(deals.moves_hours(h(d1)));
    EXPECT_TRUE(deals.counts(h(d2)));
}

// Перемещение между своими ветками: бумага уходит в другой кошелёк и тратится
// оттуда, а из прежнего её больше нет.
TEST_F(SettlementTableTest, AMoveCarriesTheBillToAnotherPurse) {
    const Block src = vera_pays(boris_, 10.0);
    records::Transfer mv{};
    mv.from      = boris_.bytes;
    mv.to        = boris_.bytes;
    mv.to_node   = kSafe;
    mv.origins   = {bill(vera_, 10.0, ref(vera_, src))};
    mv.timestamp = kT0 + 200;
    const Block moved = put(boris_, mv);                         // из кошелька kLeaf

    const Block w1 = job(anna_, boris_, 4.0);
    records::Transfer pay = offer(boris_, anna_, w1, {bill(vera_, 4.0, ref(boris_, moved))}, kT0 + 300);
    const Block d1 = put(boris_, pay, kSafe);                    // платит сейф
    answer(anna_, d1, boris_, true, kT0 + 350);
    const Block w2 = job(petr_, boris_, 4.0);
    const Block d2 = put(boris_, offer(boris_, petr_, w2,
                                       {bill(vera_, 4.0, ref(vera_, src), ref(boris_, moved))},
                                       kT0 + 400));              // из старого — уже нечем
    answer(petr_, d2, boris_, true, kT0 + 450);

    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.moves_backed.count(h(moved)));
    EXPECT_TRUE(deals.counts(h(d1)));
    EXPECT_FALSE(deals.moves_hours(h(d2)));
}

// Выкуп по заранее подписанному возврату не ждёт ответа, если платит бумагой
// самого держателя или своей; чужой бумагой — ждёт, как любая сделка.
TEST_F(SettlementTableTest, ABuybackWithAgreedPaperNeedsNoAnswer) {
    records::WorkRecord make{};
    make.hours   = 4.0;
    make.outputs = {{"палатка", 1.0, "шт"}};
    make.axes    = {{test_axis(), 1.0}};
    const Block work = put(anna_, make);
    records::Release rl{};
    rl.work      = ref(anna_, work);
    rl.to        = boris_.bytes;
    rl.timestamp = kT0 + 50;
    rl.expires   = kT0 + 5000;
    rl.addressed = records::Addressed{anna_.bytes, kT0 + 9000};
    const Block rb = put(anna_, rl);
    records::Transfer buy = offer(boris_, anna_, work, {{boris_.bytes, 4.0}}, kT0 + 100);
    buy.accept->release = ref(anna_, rb);
    buy.accept->buyback = records::Buyback{anna_.bytes, kT0 + 9000};
    const Block held = put(boris_, buy);
    answer(anna_, held, boris_, true);

    // Анна выкупает бумагой Бориса — той, что пришла ей с покупкой.
    records::Acceptance ba{};
    ba.work          = ref(anna_, work);
    ba.release       = ref(boris_, held);
    ba.carried_units = 4.0;
    records::Transfer back{};
    back.from      = anna_.bytes;
    back.to        = boris_.bytes;
    back.to_node   = kLeaf;
    back.origins   = {bill(boris_, 4.0, ref(boris_, held))};
    back.accept    = ba;
    back.timestamp = kT0 + 600;
    back.until     = kNow;
    const Block bb = put(anna_, back);                           // Борис не отвечает

    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.pre_accepted.count(h(bb)));
    EXPECT_TRUE(deals.moves_hours(h(bb)));
    EXPECT_TRUE(deals.bought_back.count(h(held)));
}

// Выкуп из залога (records.md §9.8 v9) ответа не ждёт: держатель согласился на
// него, когда клал залог, — ему возвращается то самое, чем он платил. Без залога
// та же чужая бумага ждёт ответа, как любая сделка.
TEST_F(SettlementTableTest, ABuybackFromTheDepositNeedsNoAnswer) {
    const Block held = tent_on_return(true);
    const Block bb   = buy_back(held, {bill(vera_, 4.0, ref(boris_, held))});   // Борис молчит
    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.pre_accepted.count(h(bb)));
    EXPECT_TRUE(deals.moves_hours(h(bb)));
    EXPECT_TRUE(deals.bought_back.count(h(held)));
}

TEST_F(SettlementTableTest, WithoutADepositForeignPaperWaitsForTheAnswer) {
    const Block held = tent_on_return(false);
    const Block bb   = buy_back(held, {bill(vera_, 4.0, ref(boris_, held))});
    const auto deals = read_deals(*storage_);
    EXPECT_FALSE(deals.pre_accepted.count(h(bb)));
    EXPECT_FALSE(deals.moves_hours(h(bb)));
}

// До срока залог заморожен: первая его трата — выкуп, а трата на постороннее —
// не бумага. После срока — свободен.
TEST_F(SettlementTableTest, TheDepositIsFrozenUntilTheTerm) {
    const Block held  = tent_on_return(true);
    const Block w     = job(petr_, anna_, 4.0);
    const Block spent = put(anna_, offer(anna_, petr_, w, {bill(vera_, 4.0, ref(boris_, held))},
                                         kT0 + 500));
    answer(petr_, spent, anna_, true, kT0 + 550);
    const auto deals = read_deals(*storage_);
    EXPECT_FALSE(deals.moves_hours(h(spent)));
    EXPECT_EQ(deals.unbacked_why.at(h(spent)),
              "залог заморожен до срока возврата: первым его тратит выкуп");
}

TEST_F(SettlementTableTest, PastTheTermTheDepositIsFree) {
    const Block held  = tent_on_return(true);
    const Block w     = job(petr_, anna_, 4.0);
    const Block spent = put(anna_, offer(anna_, petr_, w, {bill(vera_, 4.0, ref(boris_, held))},
                                         kT0 + 9500));
    answer(petr_, spent, anna_, true, kT0 + 9550);
    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.counts(h(spent)));
}

// ── Долг эмитента при рождении (economy.md §4.4) ─────────────────────────────

// У любого векселя виден долг эмитента в момент, когда родилась его бумага:
// назад по векселям до самоэмиссии, сколько бы рук ни было между. Вексель,
// собранный из двух, — смесь двух рождений.
TEST_F(SettlementTableTest, EveryBillShowsWhereItsPaperWasBorn) {
    const Block w1 = job(boris_, vera_, 2.0);
    records::Transfer t1 = offer(vera_, boris_, w1, {{vera_.bytes, 2.0}});
    t1.emission = records::EmissionLink{0, std::nullopt, -4.0};       // долг Веры после — 4ч
    const Block d1 = put(vera_, t1);
    answer(boris_, d1, vera_, true);
    const Block w2 = job(boris_, vera_, 6.0);
    records::Transfer t2 = offer(vera_, boris_, w2, {{vera_.bytes, 6.0}});
    t2.emission = records::EmissionLink{1, records::Ref{vera_.bytes, h(d1)}, -12.0};   // — 12ч
    const Block d2 = put(vera_, t2);
    answer(boris_, d2, vera_, true);
    // Борис платит Анне 4ч бумаги Веры: по 2ч из каждого векселя.
    const Block w3 = job(anna_, boris_, 4.0);
    const Block d3 = put(boris_, offer(boris_, anna_, w3, {bill(vera_, 2.0, ref(vera_, d1)),
                                                           bill(vera_, 2.0, ref(vera_, d2))}));
    answer(anna_, d3, boris_, true);

    const auto deals = read_deals(*storage_);
    const auto& b1 = deals.births.at(BillKey{boris_.bytes, vera_.bytes, h(d1), vera_.bytes});
    ASSERT_EQ(b1.parts.size(), 1u);
    EXPECT_DOUBLE_EQ(b1.parts[0].first, 4.0);
    EXPECT_DOUBLE_EQ(b1.parts[0].second, 2.0);
    const auto& b3 = deals.births.at(BillKey{anna_.bytes, boris_.bytes, h(d3), vera_.bytes});
    ASSERT_EQ(b3.parts.size(), 2u);
    EXPECT_DOUBLE_EQ(b3.parts[0].first, 4.0);
    EXPECT_DOUBLE_EQ(b3.parts[0].second, 2.0);
    EXPECT_DOUBLE_EQ(b3.parts[1].first, 12.0);
    EXPECT_DOUBLE_EQ(b3.parts[1].second, 2.0);
    EXPECT_DOUBLE_EQ(b3.unknown, 0.0);
}

// Источник, которого нет на столе, — рождение неизвестно, а не выдумано.
TEST_F(SettlementTableTest, ABirthOffTheTableIsUnknown) {
    const Block w = job(anna_, boris_, 3.0);
    records::Ref nowhere{vera_.bytes, {}};
    nowhere.hash.fill(0xEE);
    const Block d = put(boris_, offer(boris_, anna_, w, {bill(vera_, 3.0, nowhere)}));
    const auto deals = read_deals(*storage_);
    const auto& b = deals.births.at(BillKey{anna_.bytes, boris_.bytes, h(d), vera_.bytes});
    EXPECT_TRUE(b.parts.empty());
    EXPECT_DOUBLE_EQ(b.unknown, 3.0);
}

// ── Подтверждённое время (blockchain.md §5.2, economy.md §8.14) ──────────────

// Молчание продавца после срока: насколько твёрдо оферта умерла. Кошелёк не ушёл
// за срок — не видно; ушёл без ответа — видно агрегатору; эту точку закрепило
// двустороннее слияние — зафиксировано. Согласие задним числом после такой
// точки — поздно: ответ не раньше своей ветки.
TEST_F(SettlementTableTest, ALapseIsAsSureAsTheSellersPurseIsFixed) {
    const Block w = put_at(gleb_, work_for(boris_, 3.0), kLeaf, 0);
    const Block d = put(boris_, offer(boris_, gleb_, w, {{boris_.bytes, 3.0}}, kT0 + 100,
                                      kT0 + 300));
    EXPECT_EQ(read_deals(*storage_).lapse.at(h(d)), "none");
    const Block s = stub(gleb_, 1, kT0 + 400);
    EXPECT_EQ(read_deals(*storage_).lapse.at(h(d)), "seen");
    const Block pt = stub(petr_, 0, kT0 + 410);
    merge(petr_, 1, s, kT0 + 420);                 // Пётр взял блок Глеба…
    merge(gleb_, 2, pt, kT0 + 420);                // …и Глеб — блок Петра
    auto deals = read_deals(*storage_);
    EXPECT_EQ(deals.lapse.at(h(d)), "witnessed");
    EXPECT_EQ(deals.witnessed(gleb_, kLeaf, 1), std::optional<int64_t>(kT0 + 420));
    EXPECT_EQ(deals.witnessed(gleb_, kLeaf, 0), std::optional<int64_t>(kT0 + 420));
    EXPECT_EQ(deals.witnessed(gleb_, kLeaf, 2), std::nullopt);

    put_at(gleb_, reply(d, boris_, true, kT0 + 200), kLeaf, 3);   // «принял» задним числом
    deals = read_deals(*storage_);
    EXPECT_EQ(deals.answer(h(d)), records::Answer::none);
    EXPECT_FALSE(deals.moves_hours(h(d)));
}

// Половина слияния с со-подписью партнёра (он прочёл черновик и нашёл время
// «сейчас», blockchain.md §6.4) — свидетельство сама: она закрепляет и ветку
// партнёра до взятого блока, и свою — до самой себя; встречной половины не надо.
TEST_F(SettlementTableTest, ACoSignedHalfFixesBothBranches) {
    const Block w = put_at(gleb_, work_for(boris_, 3.0), kLeaf, 0);
    const Block d = put(boris_, offer(boris_, gleb_, w, {{boris_.bytes, 3.0}}, kT0 + 100,
                                      kT0 + 300));
    stub(gleb_, 1, kT0 + 400);
    const Block pt = stub(petr_, 0, kT0 + 410);
    const Block g2 = merge(gleb_, 2, pt, kT0 + 420);            // только половина Глеба
    EXPECT_EQ(read_deals(*storage_).lapse.at(h(d)), "seen");
    Signature cosig{};
    cosig.bytes.fill(0x5C);                                     // проверяют её на входе
    ASSERT_TRUE(storage_->put_cosig(Crypto::hash_block(g2), cosig));
    const auto deals = read_deals(*storage_);
    EXPECT_EQ(deals.witnessed(petr_, kLeaf, 0), std::optional<int64_t>(kT0 + 420));
    EXPECT_EQ(deals.witnessed(gleb_, kLeaf, 2), std::optional<int64_t>(kT0 + 420));
    EXPECT_EQ(deals.lapse.at(h(d)), "witnessed");
}

// Блок из будущего (blockchain.md §9.4): заявляет время позже, чем доказуемо
// существовал, — пришёл сюда раньше или раньше закреплён слиянием.
TEST_F(SettlementTableTest, ABlockFromTheFutureIsSeen) {
    const auto now      = static_cast<Timestamp>(std::time(nullptr));
    const Block ahead   = stub(gleb_, 0, now + 86'400);         // пришёл раньше, чем «написан»
    const Block present = stub(anna_, 0, now - 100);
    const Block pt      = stub(petr_, 0, now - 7'200);          // Пётр закрепил блок Глеба…
    const Block g1      = stub(gleb_, 1, now - 3'600 + 86'400); // …а тот назвался днём позже
    merge(petr_, 1, g1, now - 3'600);
    merge(gleb_, 2, pt, now - 3'600);
    const auto deals = read_deals(*storage_);
    EXPECT_TRUE(deals.from_future.count(h(ahead)));
    EXPECT_TRUE(deals.from_future.count(h(g1)));
    EXPECT_FALSE(deals.from_future.count(h(present)));
    EXPECT_FALSE(deals.from_future.count(h(pt)));
}

// Слияние своих веток и половина слияния ничего не закрепляют: подписанное одной
// рукой против этой руки не свидетельствует. И дыра в кошельке до точки — тоже
// «не видно»: в пропущенном блоке мог быть ответ.
TEST_F(SettlementTableTest, OnlyABilateralMergeWithAnotherFixesTime) {
    const Block w = put_at(gleb_, work_for(boris_, 3.0), kLeaf, 0);
    const Block d = put(boris_, offer(boris_, gleb_, w, {{boris_.bytes, 3.0}}, kT0 + 100,
                                      kT0 + 300));
    const Block s   = stub(gleb_, 1, kT0 + 400);
    const Block own = stub(gleb_, 0, kT0 + 405, kSafe);
    merge(gleb_, 1, s, kT0 + 420, kSafe);           // своя ветка взяла блок кошелька…
    const Block g2 = merge(gleb_, 2, own, kT0 + 420);   // …и кошелёк — блок своей ветки
    stub(petr_, 0, kT0 + 410);
    merge(petr_, 1, g2, kT0 + 430);                 // Пётр взял, а ответной половины нет
    const auto deals = read_deals(*storage_);
    EXPECT_EQ(deals.witnessed(gleb_, kLeaf, 1), std::nullopt);
    EXPECT_EQ(deals.lapse.at(h(d)), "seen");

    // У Анны кошелёк ушёл за срок, но блоков 1…3 на столе нет.
    const Block w2 = put_at(anna_, work_for(boris_, 2.0), kLeaf, 0);
    const Block d2 = put(boris_, offer(boris_, anna_, w2, {{boris_.bytes, 2.0}}, kT0 + 100,
                                       kT0 + 300));
    stub(anna_, 4, kT0 + 400);
    EXPECT_EQ(read_deals(*storage_).lapse.at(h(d2)), "none");
}

// Сторно своей сделки, которую продавец на деле принял, — объявленный прочь долг:
// две подписи противоречат друг другу, и это видно в следе.
TEST_F(SettlementTableTest, AReversalOfAnAcceptedDealIsSeen) {
    const Block w = job(anna_, boris_, 3.0);
    const Block d = put(boris_, offer(boris_, anna_, w, {{boris_.bytes, 3.0}}));
    answer(anna_, d, boris_, true);
    records::Redemption rd{};
    rd.transfer        = ref(boris_, d);
    rd.units           = 3.0;
    rd.link.seq        = 1;
    rd.link.debt_after = 0.0;
    rd.timestamp       = kT0 + 900;
    put(boris_, rd);
    const auto view = FootprintView::build(*storage_);
    const auto b = view.chain(boris_);
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(b->false_reversals, 1u);
}

// ── Один стол на состояние хранилища (ускорение, records.md §13) ─────────────

// Стол пересчитывается, только когда в хранилище появились новые блоки: пока
// их число стоит, читатели получают тот же самый стол.
TEST_F(SettlementTableTest, TheTableIsSharedUntilABlockArrives) {
    const Block w = job(anna_, boris_, 3.0);
    const auto  first  = deals_on_table(*storage_);
    const auto  second = deals_on_table(*storage_);
    EXPECT_EQ(first.get(), second.get());                       // тот же стол
    const Block d = put(boris_, offer(boris_, anna_, w, {{boris_.bytes, 3.0}}));
    const auto  third = deals_on_table(*storage_);
    EXPECT_NE(third.get(), first.get());                        // новый блок — новый стол
    ASSERT_EQ(third->all.size(), 1u);
    EXPECT_EQ(third->all[0].hash, h(d));
    EXPECT_TRUE(first->all.empty());                            // старый не тронут
}

// Блок разбирается один раз: ссылка устойчива, адрес и запись — как в блоке.
TEST_F(SettlementTableTest, ABlockIsDecodedOnce) {
    const Block w  = job(anna_, boris_, 3.0);
    const auto& a1 = seen_block(*storage_, Crypto::hash_block(w));
    const auto& a2 = seen_block(*storage_, Crypto::hash_block(w));
    EXPECT_EQ(&a1, &a2);
    EXPECT_TRUE(a1.data);
    EXPECT_EQ(a1.address, w.address);
    EXPECT_TRUE(std::holds_alternative<records::WorkRecord>(a1.record));
}

// Блока ещё нет — запоминать нечего: когда он придёт, его увидят. Слияние
// называет блок партнёра, которого на столе может пока не быть.
TEST_F(SettlementTableTest, AMissIsNotRemembered) {
    records::WorkRecord w = work_for(boris_, 3.0);
    w.start_ts = kT0;
    Block b{};
    b.address           = {anna_, kLeaf, 0};
    b.prev_hash         = Hash::zero();
    b.timestamp_claimed = kT0 + 10800;
    b.type              = BlockType::DATA;
    b.payload           = records::Codec::encode(w);
    b.signature         = Signature::null();
    const auto    bytes = Serializer::encode(b);
    const KeyPair kp    = Crypto::generate_keypair();
    b.signature = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    const Hash bh = Crypto::hash_block(b);
    EXPECT_FALSE(seen_block(*storage_, bh).data);               // ещё нет
    ASSERT_TRUE(storage_->add_block(b));
    EXPECT_TRUE(seen_block(*storage_, bh).data);                // пришёл — и виден
}

// У каждого хранилища свой номер: кэш одного не достаётся другому, даже если
// число блоков совпало.
TEST(StorageIdentity, EveryStorageHasItsOwnInstance) {
    const auto base = std::filesystem::temp_directory_path();
    std::filesystem::remove_all(base / "bc_identity_a");
    std::filesystem::remove_all(base / "bc_identity_b");
    {
        AggregatorStorage a(base / "bc_identity_a");
        AggregatorStorage b(base / "bc_identity_b");
        EXPECT_NE(a.instance_id(), b.instance_id());
        EXPECT_NE(deals_on_table(a).get(), deals_on_table(b).get());
    }
    std::filesystem::remove_all(base / "bc_identity_a");
    std::filesystem::remove_all(base / "bc_identity_b");
}
