#include "aggregator/cloud_view.h"
#include <records/goods_thread.h>
#include "labor_thread_support.h"
#include "settlement_support.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

using namespace aggregator;
using namespace blockchain;

namespace {

// Ось — по хешу её определяющего блока. Как она называется, здесь неважно:
// личность оси — запись (ИР-022).
records::BlockPath axis(uint8_t tag) {
    records::BlockPath p{};
    p.chain.fill(0x79);
    p.hash.fill(tag);
    return p;
}
constexpr uint8_t kDanger = 0x01, kPrecision = 0x02, kPhysical = 0x03,
                  kPeople = 0x04, kKnowledge = 0x05;

std::array<uint8_t, 32> id(uint8_t tag) { return axis(tag).hash; }

SampleKey key(uint8_t sample) {
    SampleKey k{};
    k.fill(sample);
    return k;
}

// Семья руками: образец по тегу, состав цены — ось → доля.
std::pair<const SampleKey, WorkComposition> family(
        uint8_t sample, std::initializer_list<std::pair<uint8_t, double>> shares,
        double carried = 0.0, const std::string& title = "") {
    WorkComposition c{};
    c.sample.chain.fill(0xB0);
    c.sample.hash = key(sample);
    c.title       = title;
    for (const auto& [tag, s] : shares) c.axes[id(tag)] = s;
    c.carried = carried;
    c.payable = 1.0;
    c.deals   = 2;
    return {key(sample), c};
}

const records::CloudPoint* point(const records::WorkCloud& c, uint8_t sample) {
    for (const auto& p : c.points)
        if (p.sample.hash == key(sample)) return &p;
    return nullptr;
}

UserId chain_of(uint8_t fill) {
    UserId u{};
    u.bytes.fill(fill);
    return u;
}

records::Ref ref_to(const UserId& chain, const Block& block) {
    records::Ref r{};
    r.chain = chain.bytes;
    r.hash  = Crypto::hash_block(block).bytes;
    return r;
}

}  // namespace

// ── Семьи образцов читаются из сделок ─────────────────────────────────────────

struct Line { uint8_t axis; double units; };
struct Opt {
    std::optional<records::Ref> like;
    std::string                 title;
    double                      carried = 0.0;
    bool                        settle  = true;
    double                      labour  = -1.0;   // ≥ 0: разбивка с ценой не сходится
};

class CompositionTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    LaborThreads threads_;   // records.md §9.4 v4: works go into the thread
    UserId                             worker_ = chain_of(0xA1);

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_cloud_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    // Через расчёт (economy.md §4.4): срок ответа, векселя и ответ продавца —
    // как у клиентов (settlement_support.h).
    Block add(const UserId& owner, const records::Record& written) {
        const records::Record rec = settle_.stamp(owner.bytes, written);
        const Block b = add_one(owner, rec);
        if (const auto r = settle_.placed(owner.bytes, rec, Crypto::hash_block(b).bytes)) {
            UserId seller{};
            seller.bytes = r->first;
            add_one(seller, r->second);
        }
        return b;
    }
    Settlement settle_;

    Block add_one(const UserId& owner, const records::Record& written) {
        const records::Record rec = threads_.stamp(owner.bytes, written);
        Block b{};
        b.address           = {owner, 0x7FFF'FFFFu, next_index_++};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 0;
        // Блок с записью труда не датируется раньше конца самой работы
        // (records.md §9.4 v5): будущих часов не заявляют.
        if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            const auto end = static_cast<Timestamp>(records::labor_interval(*w).second);
            if (end > b.timestamp_claimed) b.timestamp_claimed = end;
        }
        b.type              = BlockType::DATA;
        b.payload           = records::Codec::encode(rec);
        b.signature         = Signature::null();
        const auto bytes    = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        EXPECT_TRUE(storage_->add_block(b));
        threads_.placed(owner.bytes, rec, Crypto::hash_block(b).bytes);
        return b;
    }

    // Одна сделка работника `worker_`: разбивка `lines` и есть цена труда.
    // Возвращает ссылку на приёмку — её можно назвать образцом в следующей.
    records::Ref deal(const UserId& payer, double hours, const std::vector<Line>& lines,
                      const Opt& o = {}) {
        double named = 0.0;
        for (const auto& l : lines) named += l.units;
        const double lab = o.labour < 0.0 ? named : o.labour;

        records::WorkRecord wr{};
        wr.hours = hours;
        wr.title = o.title;
        wr.customer = payer.bytes;   // на заказ плательщика (records.md §9.4)
        const Block work = add(worker_, wr);

        records::Acceptance a{};
        a.work        = ref_to(worker_, work);
        a.hours_raw   = hours;
        a.labor_units = lab;
        for (const auto& l : lines) a.axes.push_back({axis(l.axis), l.units});
        if (o.carried > 0.0) a.carried_units = o.carried;
        a.like        = o.like;

        // Приёмка — внутри своей оплаты (records.md §9.5, ИР-023): неоплаченной
        // приёмки больше не бывает — без оплаты сделки просто нет.
        if (!o.settle) return records::Ref{};
        records::Transfer t{};
        t.from      = payer.bytes;
        t.to        = worker_.bytes;
        t.origins   = { {payer.bytes, lab + o.carried} };
        t.accept    = a;
        t.timestamp = 86'400LL * 100;
        return ref_to(payer, add(payer, t));
    }
};

// Образец, который никто не повторил, — просто сделка, а не точка облака.
TEST_F(CompositionTest, ASampleNobodyRepeatedIsNotAPoint) {
    deal(chain_of(0xB2), 4.0, {{kDanger, 8.0}}, {std::nullopt, "сварщик"});
    EXPECT_TRUE(build_work_compositions(*storage_).empty());
}

// Семья — образец и все, кто на него сослался, напрямую или через того, кто
// сослался. Название — у образца, а не у последователя: вольный текст, по
// которому ничего не считается.
TEST_F(CompositionTest, TheFamilyIsTheSampleAndEveryoneWhoNamedIt) {
    const auto s = deal(chain_of(0xB2), 4.0, {{kDanger, 3.0}, {kPhysical, 5.0}},
                        {std::nullopt, "сварщик"});
    const auto a = deal(chain_of(0xB3), 2.0, {{kDanger, 1.0}, {kPhysical, 3.0}},
                        {s, "сварка труб"});
    deal(chain_of(0xB4), 1.0, {{kDanger, 0.5}, {kPhysical, 0.5}}, {a});   // через a — к s

    const auto comps = build_work_compositions(*storage_);
    ASSERT_EQ(comps.size(), 1u);
    const auto& c = comps.begin()->second;
    EXPECT_EQ(comps.begin()->first, s.hash);
    EXPECT_EQ(c.sample, s);
    EXPECT_EQ(c.title, "сварщик");
    EXPECT_EQ(c.deals, 3u);
    EXPECT_DOUBLE_EQ(c.axes.at(id(kDanger)),   4.5 / 13.0);
    EXPECT_DOUBLE_EQ(c.axes.at(id(kPhysical)), 8.5 / 13.0);
}

// Доли — суммы по семье, делённые на всё уплаченное, а не среднее долей сделок:
// короткая работа и долгая не получают равного голоса. Как средний трудочас.
TEST_F(CompositionTest, SharesAreWhatWasPaidNotAMeanOfDeals) {
    const auto s = deal(chain_of(0xB2), 4.0, {{kDanger, 3.0}, {kPhysical, 5.0}});
    Opt o;
    o.like    = s;
    o.carried = 1.0;
    deal(chain_of(0xB3), 1.0, {{kDanger, 0.5}, {kPhysical, 0.5}}, o);   // 1 стч + 1 материалы

    const auto comps = build_work_compositions(*storage_);
    ASSERT_EQ(comps.size(), 1u);
    const auto& c = comps.begin()->second;
    EXPECT_DOUBLE_EQ(c.payable, 10.0);
    EXPECT_DOUBLE_EQ(c.axes.at(id(kDanger)),   3.5 / 10.0);   // среднее долей было бы 0.3125
    EXPECT_DOUBLE_EQ(c.axes.at(id(kPhysical)), 5.5 / 10.0);   // …и 0.4375
    EXPECT_DOUBLE_EQ(c.carried,                1.0 / 10.0);   // …и 0.25
}

// Не входит то же, что не входит в книгу осей: разбивка, не сходящаяся с ценой.
// Неоплаченной приёмки и самосделки больше нельзя даже записать — приёмка едет
// внутри своей оплаты (records.md §9.5). Такой «последователь» семью не образует.
TEST_F(CompositionTest, DealsThatDoNotCountAreLeftOut) {
    const auto s = deal(chain_of(0xB2), 4.0, {{kDanger, 8.0}});
    Opt unpaid;  unpaid.like = s;  unpaid.settle = false;
    Opt skewed;  skewed.like = s;  skewed.labour = 8.0;
    deal(chain_of(0xB3), 4.0, {{kDanger, 8.0}}, unpaid);       // не записана вовсе
    EXPECT_THROW(deal(worker_, 4.0, {{kDanger, 8.0}}, {s}),    // самосделку не записать
                 records::CodecError);
    deal(chain_of(0xB4), 4.0, {{kDanger, 5.0}}, skewed);       // названо 5 из 8
    EXPECT_TRUE(build_work_compositions(*storage_).empty());
}

// Ставка — высота над картой, а не её координата (specialty-axes.md §7). Двум
// образцам платят за одно и то же, одному втрое дороже — это одна точка.
TEST_F(CompositionTest, RateIsAHeightNotACoordinate) {
    const auto roof = deal(chain_of(0xB2), 10.0, {{kDanger, 4.0}, {kPhysical, 6.0}});
    deal(chain_of(0xB3), 10.0, {{kDanger, 4.0}, {kPhysical, 6.0}}, {roof});
    const auto tin = deal(chain_of(0xB4), 10.0, {{kDanger, 12.0}, {kPhysical, 18.0}});
    deal(chain_of(0xB5), 10.0, {{kDanger, 12.0}, {kPhysical, 18.0}}, {tin});

    const auto comps = build_work_compositions(*storage_);
    ASSERT_EQ(comps.size(), 2u);
    EXPECT_NEAR(composition_affinity(comps.at(roof.hash), comps.at(tin.hash)), 1.0, 1e-12);
}

// Образца может и не быть в хранилище — семья вокруг него собирается всё равно:
// точка — это то, на что ссылаются, а не то, что агрегатор успел увидеть.
TEST_F(CompositionTest, AnUnseenSampleStillGathersItsFamily) {
    records::Ref far{};
    far.chain.fill(0xCC);
    far.hash.fill(0xCD);
    deal(chain_of(0xB2), 2.0, {{kPhysical, 2.0}}, {far, "повар"});
    deal(chain_of(0xB3), 3.0, {{kPhysical, 3.0}}, {far, "повар"});

    const auto comps = build_work_compositions(*storage_);
    ASSERT_EQ(comps.size(), 1u);
    EXPECT_EQ(comps.begin()->second.sample, far);
    EXPECT_EQ(comps.begin()->second.deals, 2u);
    EXPECT_EQ(comps.begin()->second.title, "повар");
}

// ── Соседство ──────────────────────────────────────────────────────────────────

// Соседи — те, кому платят за похожее. Электрик ближе всего к монтёру (опасность
// и точность), дальше — к сварщику (общая только опасность). С учителем у него нет
// ничего общего — и учитель ему не «далёкий сосед», а не сосед вовсе.
TEST(CloudView, NeighboursArePaidForTheSameThings) {
    const std::map<SampleKey, WorkComposition> comps = {
        family(0x10, {{kDanger, 0.4}, {kPrecision, 0.6}}, 0.0, "электрик"),
        family(0x11, {{kDanger, 0.5}, {kPrecision, 0.5}}, 0.0, "монтёр"),
        family(0x12, {{kDanger, 0.5}, {kPhysical,  0.5}}, 0.0, "сварщик"),
        family(0x13, {{kPeople, 0.7}, {kKnowledge, 0.3}}, 0.0, "учитель"),
    };
    const auto cloud = build_work_cloud(comps, 0, 0, {});
    EXPECT_EQ(cloud.points.size(), 4u);

    const auto* el = point(cloud, 0x10);
    ASSERT_NE(el, nullptr);
    EXPECT_EQ(el->title, "электрик");
    EXPECT_EQ(el->deals, 2u);
    ASSERT_EQ(el->neighbors.size(), 2u);
    EXPECT_EQ(el->neighbors[0].sample.hash, key(0x11));
    EXPECT_NEAR(el->neighbors[0].weight, std::sqrt(0.2) + std::sqrt(0.3), 1e-12);
    EXPECT_EQ(el->neighbors[1].sample.hash, key(0x12));
    EXPECT_NEAR(el->neighbors[1].weight, std::sqrt(0.2), 1e-12);

    const auto* t = point(cloud, 0x13);
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->neighbors.empty());
}

// Одна и та же «опасность», заведённая в двух цепях, — две разные оси: личность
// оси — запись, а не слаг (ИР-022). Облако их не сливает, как не сливает и книга
// осей: заявка `same_as` сама не применяется.
TEST(CloudView, SameNameFromAnotherDefinitionIsAnotherAxis) {
    auto danger_elsewhere = axis(kDanger);
    danger_elsewhere.hash.fill(0xD0);
    WorkComposition a{}, b{};
    a.axes[axis(kDanger).hash]    = 1.0;
    b.axes[danger_elsewhere.hash] = 1.0;
    EXPECT_DOUBLE_EQ(composition_affinity(a, b), 0.0);
}

// Материалы — тоже то, из чего сложилась цена. Повару и пекарю платят за одно и
// то же в труде, но половина цены пекаря — мука и печь: сходство √0.5, а не 1.
TEST(CloudView, CarriedMeansAreOneMoreThingThePriceWasMadeOf) {
    const auto cook  = family(0x20, {{kPhysical, 0.6}, {kPrecision, 0.4}}).second;
    const auto baker = family(0x21, {{kPhysical, 0.3}, {kPrecision, 0.2}}, 0.5).second;
    EXPECT_NEAR(composition_affinity(cook, baker), std::sqrt(0.5), 1e-12);
    EXPECT_DOUBLE_EQ(composition_affinity(cook, baker), composition_affinity(baker, cook));
    EXPECT_NEAR(composition_affinity(baker, baker), 1.0, 1e-12);
}

// k ограничивает соседей; равные веса развязываются хешем образца, и методика
// записана в params, чтобы свидетель пересчитал так же.
TEST(CloudView, KBoundsTheNeighboursAndIsCommitted) {
    std::map<SampleKey, WorkComposition> comps;
    for (uint8_t i = 0; i < 8; ++i)
        comps.insert(family(static_cast<uint8_t>(0x30 + i),
                            {{kDanger, 0.5}, {static_cast<uint8_t>(0x40 + i), 0.5}}));
    const auto cloud = build_work_cloud(comps, 0, 0, {}, {}, 3);
    for (const auto& p : cloud.points) EXPECT_EQ(p.neighbors.size(), 3u);
    EXPECT_EQ(point(cloud, 0x30)->neighbors[0].sample.hash, key(0x31));
    EXPECT_NE(cloud.params.find("basis=samples"), std::string::npos);
    EXPECT_NE(cloud.params.find("k=3"), std::string::npos);
}

TEST(CloudView, Deterministic) {
    const std::map<SampleKey, WorkComposition> comps = {
        family(0x50, {{kDanger, 0.3}, {kPhysical, 0.7}}),
        family(0x51, {{kDanger, 0.6}, {kPrecision, 0.4}}),
        family(0x52, {{kPhysical, 0.2}, {kPrecision, 0.8}}, 0.1),
    };
    std::array<uint8_t, 32> snap{};
    snap.fill(0x09);
    const auto x = build_work_cloud(comps, 86'400, 5, snap);
    const auto y = build_work_cloud(comps, 86'400, 5, snap);
    ASSERT_EQ(x.points.size(), y.points.size());
    for (std::size_t i = 0; i < x.points.size(); ++i) {
        EXPECT_EQ(x.points[i].sample, y.points[i].sample);
        ASSERT_EQ(x.points[i].neighbors.size(), y.points[i].neighbors.size());
        for (std::size_t j = 0; j < x.points[i].neighbors.size(); ++j)
            EXPECT_EQ(x.points[i].neighbors[j], y.points[i].neighbors[j]);
    }
}

// Спектральные координаты: детерминированы, и тем, кому платят за похожее, ближе
// друг к другу, чем к тем, кому платят за другое.
TEST(CloudView, SpectralCoordsClusterAndDeterministic) {
    const std::map<SampleKey, WorkComposition> comps = {
        family(0x60, {{kKnowledge, 0.8}, {kPrecision, 0.2}}),   // программист
        family(0x61, {{kKnowledge, 0.7}, {kPrecision, 0.3}}),   // бухгалтер
        family(0x62, {{kPeople,    0.7}, {kKnowledge, 0.3}}),   // учитель
        family(0x63, {{kDanger,    0.5}, {kPhysical,  0.5}}),   // сварщик
        family(0x64, {{kDanger, 0.4}, {kPhysical, 0.4}, {kPrecision, 0.2}}),   // монтёр
    };
    const auto a = compute_spectral_coords(comps);
    const auto b = compute_spectral_coords(comps);
    ASSERT_EQ(a.size(), 5u);
    for (const auto& [k, c] : a) {
        ASSERT_TRUE(b.count(k));
        EXPECT_DOUBLE_EQ(c[0], b.at(k)[0]);
        EXPECT_DOUBLE_EQ(c[1], b.at(k)[1]);
    }
    auto dist = [&](uint8_t x, uint8_t y) {
        const double dx = a.at(key(x))[0] - a.at(key(y))[0];
        const double dy = a.at(key(x))[1] - a.at(key(y))[1];
        return std::sqrt(dx * dx + dy * dy);
    };
    EXPECT_LT(dist(0x60, 0x61), dist(0x60, 0x63));
    EXPECT_LT(dist(0x63, 0x64), dist(0x63, 0x61));
}
