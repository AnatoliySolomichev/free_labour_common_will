#include "aggregator/attestation_view.h"
#include <records/goods_thread.h>
#include "labor_thread_support.h"
#include "settlement_support.h"

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

// Ось — по тегу хеша её определения.
records::BlockPath axis(uint8_t tag) {
    records::BlockPath p{};
    p.chain.fill(0x79);
    p.hash.fill(tag);
    return p;
}
constexpr uint8_t kLiveLine = 0x01, kDanger = 0x02;

} // namespace

// Квалификация — это принятая работа, прочитанная по осям (records.md §14.4,
// ИР-022 шаг 11): «по оси такой-то приняли N цепей, M часов». Разряда, который
// надо заверять, больше нет.
class AttestationViewTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    LaborThreads threads_;   // records.md §9.4 v4: works go into the thread

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

    // Через расчёт (economy.md §4.4): срок ответа, векселя и ответ продавца —
    // как у клиентов (settlement_support.h).
    Hash add(const UserId& owner, const records::Record& written) {
        const records::Record rec = settle_.stamp(owner.bytes, written);
        const Hash h = add_one(owner, rec);
        if (const auto r = settle_.placed(owner.bytes, rec, h.bytes)) {
            UserId seller{};
            seller.bytes = r->first;
            add_one(seller, r->second);
        }
        return h;
    }
    Settlement settle_;

    Hash add_one(const UserId& owner, const records::Record& written) {
        const records::Record rec = threads_.stamp(owner.bytes, written);
        Block b{};
        b.address           = {owner, 0x7FFF'FFFFu, next_index_++};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 1000;
        // Блок с записью труда не датируется раньше конца самой работы
        // (records.md §9.4 v5): будущих часов не заявляют.
        if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            const auto end = static_cast<Timestamp>(records::labor_interval(*w).second);
            if (end > b.timestamp_claimed) b.timestamp_claimed = end;
        }
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

    // Работа `worker` на `hours` часов, принятая `acceptor` с разбивкой по осям
    // поровну; `work_chain` — цепь, которую называет ссылка приёмки.
    void accepted_work(const UserId& worker, const UserId& acceptor, double hours,
                       std::initializer_list<uint8_t> tags,
                       const UserId* work_chain = nullptr, double named = -1.0) {
        records::WorkRecord w;
        w.action   = "работа";
        w.hours    = hours;
        w.customer = acceptor.bytes;   // заказ принимающего (records.md §9.4)
        const Hash wh = add(worker, w);
        records::Acceptance a{};
        a.work        = ref_of(work_chain ? *work_chain : worker, wh);
        a.hours_raw   = hours;
        a.labor_units = hours;
        const double each = (named < 0.0 ? hours : named) / static_cast<double>(tags.size());
        for (uint8_t tag : tags) a.axes.push_back({axis(tag), each});
        // Приёмка — внутри своей оплаты (records.md §9.5, ИР-023).
        records::Transfer t{};
        t.from      = acceptor.bytes;
        t.to        = a.work.chain;
        t.origins   = {{acceptor.bytes, hours}};
        t.accept    = a;
        t.timestamp = 1000;
        add(acceptor, t);
    }
};

// Считаются разные цепи, а не приёмки; собственная цепь работника себя не
// заверяет — такое поручительство ничего не стоит.
TEST_F(AttestationViewTest, DistinctChainsAndNoSelfVouch) {
    const UserId anna  = make_owner(0xA1);
    const UserId vera  = make_owner(0xB2);
    const UserId boris = make_owner(0xC3);

    accepted_work(anna, vera,  6, {kLiveLine});
    accepted_work(anna, vera,  4, {kLiveLine});      // та же цепь — один свидетель
    accepted_work(anna, boris, 2, {kLiveLine});
    EXPECT_THROW(accepted_work(anna, anna, 99, {kLiveLine}),   // сам себя — не записать
                 records::CodecError);

    const auto view = AttestationView::build(*storage_);
    const auto att  = view.at(anna, axis(kLiveLine).hash);
    ASSERT_TRUE(att.has_value());
    EXPECT_EQ(att->chains, 2u);                      // Вера и Борис, а не три приёмки
    EXPECT_DOUBLE_EQ(att->hours, 12.0);
    EXPECT_EQ(view.total(anna).chains, 2u);
    EXPECT_DOUBLE_EQ(view.total(anna).hours, 12.0);

    EXPECT_FALSE(view.at(anna, axis(kDanger).hash).has_value());
    EXPECT_FALSE(view.at(vera, axis(kLiveLine).hash).has_value());
    EXPECT_EQ(view.total(vera).chains, 0u);
}

// Каждая ось заверяется отдельно: работа, назвавшая две оси, свидетельствует о
// каждой. Список — по часам, самое заверенное первым.
TEST_F(AttestationViewTest, EachAxisIsAttestedSeparately) {
    const UserId anna  = make_owner(0xA1);
    const UserId vera  = make_owner(0xB2);
    const UserId boris = make_owner(0xC3);

    accepted_work(anna, vera,  5, {kLiveLine, kDanger});
    accepted_work(anna, boris, 3, {kLiveLine});

    const auto view = AttestationView::build(*storage_);
    const auto list = view.of(anna);
    ASSERT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].axis, axis(kLiveLine));
    EXPECT_DOUBLE_EQ(list[0].hours, 8.0);
    EXPECT_EQ(list[0].chains, 2u);
    EXPECT_EQ(list[1].axis, axis(kDanger));
    EXPECT_DOUBLE_EQ(list[1].hours, 5.0);
    EXPECT_EQ(list[1].chains, 1u);
    EXPECT_DOUBLE_EQ(view.total(anna).hours, 8.0);   // часы работы, не сумма по осям
}

// Работа обязана лежать в той цепи, которую называет ссылка приёмки: чужую
// работу к своей квалификации не пришьёшь.
TEST_F(AttestationViewTest, TheWorkMustSitInTheChainItsRefNames) {
    const UserId anna    = make_owner(0xA1);
    const UserId vera    = make_owner(0xB2);
    const UserId mallory = make_owner(0xEE);

    accepted_work(mallory, vera, 5, {kLiveLine}, &anna);   // блок Мэллори, ссылка — на Анну

    const auto view = AttestationView::build(*storage_);
    EXPECT_TRUE(view.of(anna).empty());
    EXPECT_TRUE(view.of(mallory).empty());
}

// Разбивка, не сходящаяся с ценой, описывает другую сделку — и ничего не заверяет.
TEST_F(AttestationViewTest, BreakdownThatDoesNotAddUpAttestsNothing) {
    const UserId anna = make_owner(0xA1);
    const UserId vera = make_owner(0xB2);
    accepted_work(anna, vera, 4, {kLiveLine}, nullptr, /*named=*/1.0);   // 1 из 4

    const auto view = AttestationView::build(*storage_);
    EXPECT_TRUE(view.of(anna).empty());
    EXPECT_EQ(view.total(anna).chains, 0u);
}

// Засчитывается сделка того, кому продавец отдал результат (records.md §9.5,
// ИР-023). Посторонний, заплативший за ту же работу, её не губит — его сделка
// ничтожна; а если продавец сам отдал работу второму покупателю, это двойная
// продажа, и не засчитывается ни одна. Погубить сделку может только продавец.
TEST_F(AttestationViewTest, OnlyTheSellerCanSpoilTheDeal) {
    const UserId anna  = make_owner(0xA1);
    const UserId vera  = make_owner(0xB2);
    const UserId boris = make_owner(0xC3);

    records::WorkRecord w;
    w.action   = "работа";
    w.hours    = 5;
    w.customer = vera.bytes;                         // заказ Веры
    const Hash wh = add(anna, w);
    const auto deal = [&](const UserId& by, std::optional<records::Ref> release) {
        records::Acceptance a{};
        a.work        = ref_of(anna, wh);
        a.hours_raw   = 5;
        a.labor_units = 5;
        a.axes        = {{axis(kLiveLine), 5.0}};
        a.release     = release;
        records::Transfer t{};
        t.from      = by.bytes;
        t.to        = anna.bytes;
        t.origins   = {{by.bytes, 5.0}};
        t.accept    = a;
        t.timestamp = 1000;
        add(by, t);
    };
    const auto attested = [&]() -> std::size_t {
        const auto att = AttestationView::build(*storage_).at(anna, axis(kLiveLine).hash);
        return att ? att->chains : 0;
    };

    deal(vera, std::nullopt);
    EXPECT_EQ(attested(), 1u);                       // Вера — приняла и заплатила

    deal(boris, std::nullopt);                       // Борис платит без адреса
    EXPECT_EQ(attested(), 1u);                       // ничтожна — сделка Веры стоит

    records::Release rl{};                           // Анна сама отдаёт работу Борису
    rl.work      = ref_of(anna, wh);
    rl.to        = boris.bytes;
    rl.timestamp = 900;
    rl.expires   = 2000;
    const Hash rh = add(anna, rl);
    deal(boris, ref_of(anna, rh));
    EXPECT_EQ(attested(), 0u);                       // двойная продажа — её, не Бориса
}
