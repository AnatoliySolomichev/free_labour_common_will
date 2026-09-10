#include "aggregator/cloud_view.h"

#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <records/codec.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
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

// Состав цены руками: ось → доля.
SpecialtyComposition comp(std::initializer_list<std::pair<uint8_t, double>> shares,
                          double carried = 0.0) {
    SpecialtyComposition c{};
    for (const auto& [tag, s] : shares) c.axes[id(tag)] = s;
    c.carried = carried;
    c.payable = 1.0;
    c.deals   = 1;
    return c;
}

const records::CloudPoint* point(const records::SpecialtyCloud& c, const std::string& slug) {
    for (const auto& p : c.points)
        if (p.slug == slug) return &p;
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

// ── Место специальности читается из сделок ────────────────────────────────────

class CompositionTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    BlockIndex                         next_index_ = 0;
    UserId                             worker_ = chain_of(0xA1);
    UserId                             payer_  = chain_of(0xB2);

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

    Block add(const UserId& owner, const records::Record& rec) {
        Block b{};
        b.address           = {owner, 0x7FFF'FFFFu, next_index_++};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 0;
        b.type              = BlockType::DATA;
        b.payload           = records::Codec::encode(rec);
        b.signature         = Signature::null();
        const auto bytes    = Serializer::encode(b);
        const KeyPair kp    = Crypto::generate_keypair();
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        EXPECT_TRUE(storage_->add_block(b));
        return b;
    }

    records::Ref grade_of(const std::string& specialty, uint8_t level) {
        const Block spec = add(worker_, records::Specialty{specialty});
        records::Grade g{};
        g.specialty = ref_to(worker_, spec);
        g.level     = level;
        return ref_to(worker_, add(worker_, g));
    }

    struct Line { uint8_t axis; double units; };

    // Одна сделка. Разбивка `lines` и есть цена труда, если `labour` не задан
    // (тогда она с ценой не сходится). `carried` — материалы сверху; `settle`
    // = false оставляет приёмку неоплаченной.
    void deal(const records::Ref& grade, const UserId& payer, double hours,
              const std::vector<Line>& lines, double carried = 0.0,
              bool settle = true, double labour = -1.0) {
        double named = 0.0;
        for (const auto& l : lines) named += l.units;
        const double lab = labour < 0.0 ? named : labour;

        records::WorkRecord wr{};
        wr.agent = grade;
        wr.hours = hours;
        const Block work = add(worker_, wr);

        records::Acceptance a{};
        a.work        = ref_to(worker_, work);
        a.receiver    = payer.bytes;
        a.hours_raw   = hours;
        a.labor_units = lab;
        for (const auto& l : lines) a.axes.push_back({axis(l.axis), l.units});
        if (carried > 0.0) a.carried_units = carried;
        a.timestamp   = 86'400LL * 100;
        const Block acc = add(payer, a);
        if (!settle) return;

        records::Transfer t{};
        t.from    = payer.bytes;
        t.to      = worker_.bytes;
        t.origins = { {payer.bytes, lab + carried} };
        t.reason  = ref_to(payer, acc);
        add(payer, t);
    }
};

// Доли — суммы по сделкам, делённые на всё уплаченное, а не среднее долей
// сделок: короткая работа и долгая не получают равного голоса в том, за что
// платят специальности. Так же, как средний трудочас — Σ стч / Σ часов.
TEST_F(CompositionTest, SharesAreWhatWasPaidNotAMeanOfDeals) {
    const auto welder = grade_of("сварщик", 4);
    deal(welder, payer_, 4.0, {{kDanger, 3.0}, {kPhysical, 5.0}});        // 8 стч
    deal(welder, payer_, 1.0, {{kDanger, 0.5}, {kPhysical, 0.5}}, 1.0);   // 1 стч + 1 материалы

    const auto comps = build_specialty_compositions(*storage_);
    ASSERT_EQ(comps.count("сварщик"), 1u);
    const auto& c = comps.at("сварщик");
    EXPECT_EQ(c.deals, 2u);
    EXPECT_DOUBLE_EQ(c.payable, 10.0);
    EXPECT_DOUBLE_EQ(c.axes.at(id(kDanger)),   3.5 / 10.0);   // среднее долей было бы 0.3125
    EXPECT_DOUBLE_EQ(c.axes.at(id(kPhysical)), 5.5 / 10.0);   // …и 0.4375
    EXPECT_DOUBLE_EQ(c.carried,                1.0 / 10.0);   // …и 0.25
}

// Не входит то же, что не входит в книгу осей: неоплаченная приёмка,
// самосделка, разбивка, не сходящаяся с ценой.
TEST_F(CompositionTest, DealsThatDoNotCountAreLeftOut) {
    const auto welder = grade_of("сварщик", 4);
    deal(welder, payer_,  4.0, {{kDanger, 8.0}}, 0.0, /*settle=*/false);    // не оплачена
    deal(welder, worker_, 4.0, {{kDanger, 8.0}});                           // самосделка
    deal(welder, payer_,  4.0, {{kDanger, 5.0}}, 0.0, true, /*labour=*/8.0); // названо 5 из 8
    EXPECT_TRUE(build_specialty_compositions(*storage_).empty());
}

// Ставка — высота над картой, а не её координата (specialty-axes.md §7).
// Кровельщику и жестянщику платят за одно и то же, одному втрое дороже — это одна
// точка. Иначе «угадай ставку по соседям» стало бы «угадай по тем, у кого похожая
// ставка».
TEST_F(CompositionTest, RateIsAHeightNotACoordinate) {
    const auto roofer   = grade_of("кровельщик", 4);
    const auto tinsmith = grade_of("жестянщик", 4);
    deal(roofer,   payer_, 10.0, {{kDanger,  4.0}, {kPhysical,  6.0}});   // 1.0 за час
    deal(tinsmith, payer_, 10.0, {{kDanger, 12.0}, {kPhysical, 18.0}});   // 3.0 за час

    const auto comps = build_specialty_compositions(*storage_);
    EXPECT_NEAR(composition_affinity(comps.at("кровельщик"), comps.at("жестянщик")),
                1.0, 1e-12);
}

// ── Соседство ──────────────────────────────────────────────────────────────────

// Соседи — те, кому платят за похожее. Электрик ближе всего к монтёру (опасность
// и точность), дальше — к сварщику (общая только опасность). С учителем у него нет
// ничего общего — и учитель ему не «далёкий сосед», а не сосед вовсе.
TEST(CloudView, NeighboursArePaidForTheSameThings) {
    const std::map<std::string, SpecialtyComposition> comps = {
        {"электрик", comp({{kDanger, 0.4}, {kPrecision, 0.6}})},
        {"монтёр",   comp({{kDanger, 0.5}, {kPrecision, 0.5}})},
        {"сварщик",  comp({{kDanger, 0.5}, {kPhysical,  0.5}})},
        {"учитель",  comp({{kPeople, 0.7}, {kKnowledge, 0.3}})},
    };
    const auto cloud = build_specialty_cloud(comps, 0, 0, {});
    EXPECT_EQ(cloud.points.size(), 4u);

    const auto* el = point(cloud, "электрик");
    ASSERT_NE(el, nullptr);
    ASSERT_EQ(el->neighbors.size(), 2u);
    EXPECT_EQ(el->neighbors[0].slug, "монтёр");
    EXPECT_NEAR(el->neighbors[0].weight, std::sqrt(0.2) + std::sqrt(0.3), 1e-12);
    EXPECT_EQ(el->neighbors[1].slug, "сварщик");
    EXPECT_NEAR(el->neighbors[1].weight, std::sqrt(0.2), 1e-12);

    const auto* t = point(cloud, "учитель");
    ASSERT_NE(t, nullptr);
    EXPECT_TRUE(t->neighbors.empty());
}

// Одна и та же «опасность», заведённая в двух цепях, — две разные оси: личность
// оси — запись, а не слаг (ИР-022). Облако их не сливает, как не сливает и книга
// осей: заявка `same_as` сама не применяется.
TEST(CloudView, SameNameFromAnotherDefinitionIsAnotherAxis) {
    auto danger_elsewhere = axis(kDanger);
    danger_elsewhere.hash.fill(0xD0);
    SpecialtyComposition a{}, b{};
    a.axes[axis(kDanger).hash]     = 1.0;
    b.axes[danger_elsewhere.hash]  = 1.0;
    EXPECT_DOUBLE_EQ(composition_affinity(a, b), 0.0);
}

// Материалы — тоже то, из чего сложилась цена. Повару и пекарю платят за одно и
// то же в труде, но половина цены пекаря — мука и печь: сходство √0.5, а не 1.
// Так капиталоёмкость живёт дальше, без веса, который пришлось бы назначить.
TEST(CloudView, CarriedMeansAreOneMoreThingThePriceWasMadeOf) {
    const auto cook  = comp({{kPhysical, 0.6}, {kPrecision, 0.4}});
    const auto baker = comp({{kPhysical, 0.3}, {kPrecision, 0.2}}, 0.5);
    EXPECT_NEAR(composition_affinity(cook, baker), std::sqrt(0.5), 1e-12);
    EXPECT_DOUBLE_EQ(composition_affinity(cook, baker), composition_affinity(baker, cook));
    EXPECT_NEAR(composition_affinity(baker, baker), 1.0, 1e-12);
}

// Родитель берётся из каталога и только как запасной ход приора; соседство им не
// задаётся. Без каталога облако строится всё равно.
TEST(CloudView, ParentComesFromTheCatalogIfOneIsServed) {
    const std::map<std::string, SpecialtyComposition> comps = {
        {"prof.welder", comp({{kDanger,   1.0}})},
        {"prof.cook",   comp({{kPhysical, 1.0}})},
    };
    records::CatalogEntry e;
    e.slug   = "prof.welder";
    e.parent = "prof.build";
    records::Catalog c;
    c.name    = "professions";
    c.entries = {e};
    const std::vector<records::Catalog> cats{c};

    const auto with = build_specialty_cloud(comps, 0, 0, {}, {}, 5, &cats);
    EXPECT_EQ(point(with, "prof.welder")->parent, "prof.build");
    EXPECT_EQ(point(with, "prof.cook")->parent, "");

    const auto without = build_specialty_cloud(comps, 0, 0, {});
    EXPECT_EQ(without.points.size(), 2u);
    EXPECT_EQ(point(without, "prof.welder")->parent, "");
}

// k ограничивает соседей; равные веса развязываются слагом, и k записан в params,
// чтобы свидетель пересчитал той же методикой.
TEST(CloudView, KBoundsTheNeighboursAndIsCommitted) {
    std::map<std::string, SpecialtyComposition> comps;
    for (int i = 0; i < 8; ++i)
        comps["s" + std::to_string(i)] =
            comp({{kDanger, 0.5}, {static_cast<uint8_t>(0x10 + i), 0.5}});
    const auto cloud = build_specialty_cloud(comps, 0, 0, {}, {}, 3);
    for (const auto& p : cloud.points) EXPECT_EQ(p.neighbors.size(), 3u);
    EXPECT_EQ(point(cloud, "s0")->neighbors[0].slug, "s1");   // все по 0.5 — по слагу
    EXPECT_NE(cloud.params.find("basis=deals"), std::string::npos);
    EXPECT_NE(cloud.params.find("k=3"), std::string::npos);
}

TEST(CloudView, Deterministic) {
    const std::map<std::string, SpecialtyComposition> comps = {
        {"a", comp({{kDanger, 0.3}, {kPhysical, 0.7}})},
        {"b", comp({{kDanger, 0.6}, {kPrecision, 0.4}})},
        {"c", comp({{kPhysical, 0.2}, {kPrecision, 0.8}}, 0.1)},
    };
    std::array<uint8_t, 32> snap{};
    snap.fill(0x09);
    const auto x = build_specialty_cloud(comps, 86'400, 5, snap);
    const auto y = build_specialty_cloud(comps, 86'400, 5, snap);
    ASSERT_EQ(x.points.size(), y.points.size());
    for (std::size_t i = 0; i < x.points.size(); ++i) {
        EXPECT_EQ(x.points[i].slug, y.points[i].slug);
        ASSERT_EQ(x.points[i].neighbors.size(), y.points[i].neighbors.size());
        for (std::size_t j = 0; j < x.points[i].neighbors.size(); ++j)
            EXPECT_EQ(x.points[i].neighbors[j], y.points[i].neighbors[j]);
    }
}

// Спектральные координаты (ИР-018 фаза 3): детерминированы, и тем, кому платят за
// похожее, ближе друг к другу, чем к тем, кому платят за другое.
TEST(CloudView, SpectralCoordsClusterAndDeterministic) {
    const std::map<std::string, SpecialtyComposition> comps = {
        {"программист", comp({{kKnowledge, 0.8}, {kPrecision, 0.2}})},
        {"бухгалтер",   comp({{kKnowledge, 0.7}, {kPrecision, 0.3}})},
        {"учитель",     comp({{kPeople,    0.7}, {kKnowledge, 0.3}})},
        {"сварщик",     comp({{kDanger,    0.5}, {kPhysical,  0.5}})},
        {"монтёр",      comp({{kDanger, 0.4}, {kPhysical, 0.4}, {kPrecision, 0.2}})},
    };
    const auto a = compute_spectral_coords(comps);
    const auto b = compute_spectral_coords(comps);
    ASSERT_EQ(a.size(), 5u);
    for (const auto& [slug, c] : a) {
        ASSERT_TRUE(b.count(slug));
        EXPECT_DOUBLE_EQ(c[0], b.at(slug)[0]);
        EXPECT_DOUBLE_EQ(c[1], b.at(slug)[1]);
    }
    auto dist = [&](const std::string& x, const std::string& y) {
        const double dx = a.at(x)[0] - a.at(y)[0];
        const double dy = a.at(x)[1] - a.at(y)[1];
        return std::sqrt(dx * dx + dy * dy);
    };
    EXPECT_LT(dist("программист", "бухгалтер"), dist("программист", "сварщик"));
    EXPECT_LT(dist("сварщик", "монтёр"),        dist("сварщик", "бухгалтер"));
}
