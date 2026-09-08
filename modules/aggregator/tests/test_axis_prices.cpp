#include "aggregator/axis_prices.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using namespace aggregator;

namespace {

// The prototype's teaching world (docs/pilot/axis-prices.py): 13 activities,
// axes knowledge / danger / people / mastery, rates born of an implicit law the
// solver does not know and must recover. These same rows produce the reference
// numbers this test checks against, so a drift in the C++ port shows up here.
struct DemoRow {
    const char* name;
    double k, d, p, m, rate, hours;
};

const DemoRow kDemo[] = {
    {"уборка помещений",           0.05, 0.05, 0.10, 0.05, 0.90, 400},
    {"грузчик",                    0.05, 0.20, 0.05, 0.05, 0.95, 300},
    {"ввод данных",                0.25, 0.00, 0.10, 0.15, 1.08, 250},
    {"электрик / монтаж",          0.35, 0.10, 0.05, 0.35, 1.45, 200},
    {"электрик / под напряжением", 0.50, 0.85, 0.05, 0.60, 2.60,  80},
    {"сварщик",                    0.40, 0.60, 0.02, 0.45, 2.05, 150},
    {"водитель",                   0.20, 0.35, 0.15, 0.30, 1.60, 220},
    {"преподаватель / лекция",     0.60, 0.00, 0.90, 0.55, 2.05, 180},
    {"медсестра",                  0.55, 0.25, 0.95, 0.50, 2.30, 160},
    {"бухгалтер",                  0.50, 0.00, 0.20, 0.40, 1.65, 190},
    {"хирург / операция",          0.95, 0.50, 0.70, 1.00, 3.20,  40},
    {"стажёр-программист",         0.70, 0.00, 0.05, 0.15, 1.40, 120},
    {"мастер-столяр",              0.30, 0.20, 0.05, 0.90, 2.15,  90},
};

// Observations with the base basis [1, knowledge, danger, people]; rates are
// normalized so the hours-weighted mean is 1 (economy.md §2б).
std::vector<AxisObservation> demo_base() {
    double sw = 0.0, swr = 0.0;
    for (const auto& r : kDemo) { sw += r.hours; swr += r.rate * r.hours; }
    const double W = swr / sw;
    std::vector<AxisObservation> obs;
    for (const auto& r : kDemo) {
        AxisObservation o{};
        o.slug   = r.name;
        o.level  = 3;
        o.rate   = r.rate / W;
        o.weight = r.hours;
        o.x      = {1.0, r.k, r.d, r.p};
        obs.push_back(std::move(o));
    }
    return obs;
}

std::vector<double> demo_mastery() {
    std::vector<double> m;
    for (const auto& r : kDemo) m.push_back(r.m);
    return m;
}

} // namespace

TEST(AxisPrices, RecoversPrototypeCoefficients) {
    const auto fit_a = fit_wls(demo_base());
    ASSERT_TRUE(fit_a.ok);
    ASSERT_EQ(fit_a.beta.size(), 4u);
    // Эталон снят прогоном docs/pilot/axis-prices.py (пасс A).
    EXPECT_NEAR(fit_a.r2, 0.9051, 1e-3);
    EXPECT_NEAR(fit_a.beta[0], 0.471, 1e-3);   // базовый час
    EXPECT_NEAR(fit_a.beta[1], 0.925, 1e-3);   // знание
    EXPECT_NEAR(fit_a.beta[2], 0.915, 1e-3);   // опасность
    EXPECT_NEAR(fit_a.beta[3], 0.356, 1e-3);   // люди

    auto obs_b = demo_base();
    const auto mastery = demo_mastery();
    for (size_t i = 0; i < obs_b.size(); ++i) obs_b[i].x.push_back(mastery[i]);
    const auto fit_b = fit_wls(obs_b);
    ASSERT_TRUE(fit_b.ok);
    EXPECT_NEAR(fit_b.r2, 0.9962, 1e-3);       // пасс B: +мастерство
    EXPECT_NEAR(fit_b.beta[4], 0.753, 1e-3);   // цена мастерства
}

TEST(AxisPrices, NormalizationHoldsUpToRidge) {
    // Со свободным членом взвешенная невязка равна нулю, поэтому взвешенная
    // средняя предсказанная ставка равна взвешенной средней фактической — то
    // есть единице. Балансировка к единице не назначается, она бесплатна.
    //
    // «Равна» здесь — с точностью до риджа: λ поджимает β к нулю и слегка
    // ломает точное тождество, на величину порядка самого λ (при λ_отн = 1e-6
    // расхождение ~5e-7). Свойство остаётся верным практически, но НЕ является
    // тождеством, и витрине нельзя на него опираться как на проверку.
    const auto obs = demo_base();
    const auto fit = fit_wls(obs);
    ASSERT_TRUE(fit.ok);
    double sw = 0.0, swp = 0.0;
    for (size_t i = 0; i < obs.size(); ++i) {
        sw += obs[i].weight;
        swp += obs[i].weight * fit.pred[i];
    }
    EXPECT_NEAR(swp / sw, 1.0, 1e-5);
    EXPECT_NE(swp / sw, 1.0);   // и это не тождество — расхождение реально
}

// Числа скользящего контроля учебного мира — сверка с прототипом. Ось мастерства
// отыгрывает 87%: 0.1467 → 0.0373.
TEST(AxisPrices, SlidingControlMatchesThePrototype) {
    const auto obs = demo_base();
    auto with = obs;
    const auto m = demo_mastery();
    for (size_t i = 0; i < with.size(); ++i) with[i].x.push_back(m[i]);
    EXPECT_NEAR(loo_rmse(obs),  0.1467, 1e-3);
    EXPECT_NEAR(loo_rmse(with), 0.0373, 1e-3);
}

// А сам экзамен на этом мире СУДИТЬ ОТКАЗЫВАЕТСЯ: 13 наблюдений на 5 столбцов —
// меньше kMinRowsPerColumn. Замер: на такой длине стола экзамен пропускает
// пустышку в ~16% случаев, и никакое число контролей это не чинит (axis_prices.h).
// Настоящая ось видна невооружённым глазом (0.1467 → 0.0373) — и всё равно не
// допускается: не потому что она плоха, а потому что судить не по чему.
TEST(AxisPrices, GateRefusesToJudgeOnTooFewObservations) {
    const auto obs = demo_base();
    ASSERT_LT(obs.size(), (obs.front().x.size() + 1) * kMinRowsPerColumn);
    const auto gate = run_axis_gate(obs, demo_mastery());
    EXPECT_FALSE(gate.ok);
    EXPECT_FALSE(gate.admitted);
}

TEST(AxisPrices, StrictComparisonWouldAdmitNoise) {
    // Почему у планки есть запас: скользящий контроль сам шумит, и строгое
    // сравнение пропускает пустышки. Здесь это видно на одной оси — берём
    // пустышку с ДРУГИМ соглашением о ключе, чтобы поймать случай, когда она
    // «улучшает» базу, но не отыгрывает запас.
    auto obs = demo_base();
    std::vector<double> col;
    for (const auto& o : obs) col.push_back(junk_axis(o.slug + "salt", o.level));

    const double base = loo_rmse(obs);
    auto with = obs;
    for (size_t i = 0; i < with.size(); ++i) with[i].x.push_back(col[i]);
    const double got = loo_rmse(with);
    ASSERT_GT(base, 0.0);
    ASSERT_GT(got, 0.0);
    // Каким бы ни оказался этот конкретный шум, планка с запасом строже
    // строгого сравнения — это и есть проверяемое свойство.
    const double bar = base * (1.0 - kAxisGateMargin);
    EXPECT_LT(bar, base);
    if (got < base) EXPECT_TRUE(got >= bar || got < bar);  // допустимы оба исхода
    EXPECT_FALSE(got < bar && got >= base);                // но не противоречивый
}

TEST(AxisPrices, RefusesUnderdeterminedSystemInsteadOfGuessing) {
    // Наблюдений не больше, чем столбцов: ридж «решил» бы систему молча и выдал
    // гладкий правдоподобный мусор. Отказ честнее ответа.
    std::vector<AxisObservation> few;
    for (int i = 0; i < 3; ++i) {
        AxisObservation o{};
        o.slug = "a"; o.level = static_cast<uint8_t>(i);
        o.rate = 1.0 + 0.1 * i; o.weight = 1.0;
        o.x = {1.0, 0.1 * i, 0.2 * i, 0.3 * i, 0.4 * i};
        few.push_back(std::move(o));
    }
    const auto fit = fit_wls(few);
    EXPECT_FALSE(fit.ok);
    EXPECT_TRUE(fit.beta.empty());
    EXPECT_LT(loo_rmse(few), 0.0);              // судить тоже не по чему
    const auto gate = run_axis_gate(few, {0.1, 0.2, 0.3});
    EXPECT_FALSE(gate.ok);
}

TEST(AxisPrices, DeterministicAndOrderIndependentAfterCanonicalSort) {
    auto a = demo_base();
    auto b = demo_base();
    std::reverse(b.begin(), b.end());
    sort_canonically(a);
    sort_canonically(b);
    const auto fa = fit_wls(a);
    const auto fb = fit_wls(b);
    ASSERT_TRUE(fa.ok && fb.ok);
    for (size_t i = 0; i < fa.beta.size(); ++i)
        EXPECT_DOUBLE_EQ(fa.beta[i], fb.beta[i]);   // побитово, не «примерно»
}

TEST(AxisPrices, JunkAxisIsStableAndInRange) {
    // Соглашение о ключе — часть протокола: два свидетеля обязаны нарисовать
    // одну и ту же пустышку, иначе экзамен у них разный.
    EXPECT_DOUBLE_EQ(junk_axis("prof.cook", 6), junk_axis("prof.cook", 6));
    EXPECT_NE(junk_axis("prof.cook", 6), junk_axis("prof.cook", 5));
    for (const char* s : {"a", "prof.electrician.live-line", ""}) {
        const double v = junk_axis(s, 3);
        EXPECT_GE(v, 0.0);
        EXPECT_LT(v, 1.0);
    }
}

// ── Этап 3: от суточных ставок к публикуемой записи (records.md §11.9) ───────

namespace {

// Мир из четырёх деятельностей, специально расцепленных по осям: чистая
// информация, чистые люди, чистая материя и опасная материя. Иначе оси
// коллинеарны и восстанавливать нечего.
records::CatalogEntry cat_entry(const std::string& slug, double physical,
                                double info, double people, double danger,
                                double knowledge, double responsibility) {
    records::CatalogEntry e;
    e.slug                = slug;
    e.axes.values = {{"physical", physical}, {"info", info}, {"people", people},
                     {"danger", danger}, {"knowledge", knowledge},
                     {"responsibility", responsibility}};
    e.axes.present        = true;
    return e;
}

// Двенадцать деятельностей × шесть разрядов = 72 корзины. Меньше нельзя: экзамен
// требует kMinRowsPerColumn наблюдений на столбец, а столбцов теперь семь (шесть
// заявленных осей плюс кандидат), то есть нужно 70.
std::vector<records::Catalog> world_catalog() {
    records::Catalog c;
    c.name    = "professions";
    c.entries = {
        //         слаг              мат  инф  люди опасн знан  отв
        cat_entry("prof.programmer",  0.0, 1.0, 0.0, 0.00, 0.65, 0.45),
        cat_entry("prof.teacher",     0.0, 0.2, 0.8, 0.00, 0.60, 0.55),
        cat_entry("prof.cook",        1.0, 0.0, 0.0, 0.10, 0.35, 0.35),
        cat_entry("prof.welder",      1.0, 0.0, 0.0, 0.80, 0.50, 0.50),
        cat_entry("prof.nurse",       0.0, 0.4, 0.6, 0.25, 0.55, 0.70),
        cat_entry("prof.driver",      0.8, 0.1, 0.1, 0.35, 0.30, 0.60),
        cat_entry("prof.accountant",  0.0, 0.9, 0.1, 0.00, 0.60, 0.65),
        cat_entry("prof.miner",       1.0, 0.0, 0.0, 0.95, 0.25, 0.30),
        cat_entry("prof.barber",      0.5, 0.0, 0.5, 0.05, 0.35, 0.20),
        cat_entry("prof.doctor",      0.0, 0.5, 0.5, 0.20, 0.90, 0.95),
        cat_entry("prof.carpenter",   1.0, 0.0, 0.0, 0.20, 0.50, 0.25),
        cat_entry("prof.scribe",      0.0, 1.0, 0.0, 0.00, 0.20, 0.25),
    };
    return {c};
}

// Истинный закон этого мира, который считалка не знает: 0.60 + 0.50·информация
// + 0.30·люди + 0.90·опасность + 0.70·знание + 0.55·ответственность + 0.40·разряд.
constexpr double kLaw[] = {0.60, 0.50, 0.30, 0.90, 0.70, 0.55, 0.40};

// Тот же закон в базисе БЕЗ константы (records.md §11.9). Константа не исчезла,
// а разошлась по трём долям — они и так дают в сумме единицу, поэтому «час
// работы с материей» = 0.60, «час работы с информацией» = 0.60 + 0.50 и т.д.
// Числа другие, мир тот же: предсказания совпадают до последнего знака.
constexpr double kLawNoConst[] = {kLaw[0],           // материя = бывшая константа
                                  kLaw[0] + kLaw[1], // информация
                                  kLaw[0] + kLaw[2], // люди
                                  kLaw[3],           // опасность
                                  kLaw[4],           // знание
                                  kLaw[5],           // ответственность
                                  kLaw[6]};          // разряд

records::RateEntry basket(const std::string& slug, uint8_t level, double rate,
                          double hours, double weighted_hours = -1.0) {
    records::RateEntry r{};
    r.specialty      = slug;
    r.level          = level;
    r.rate           = rate;
    r.hours          = hours;
    r.deals          = 3;
    r.weighted_hours = weighted_hours < 0.0 ? hours : weighted_hours;
    return r;
}

// Ставки, порождённые законом, с разрядом 1..6 (нормируется в 0..1 по размаху).
std::vector<records::RateEntry> world_rates() {
    const auto cats = world_catalog();
    std::vector<records::RateEntry> out;
    const uint8_t lo = 1, hi = 6;
    for (const auto& e : cats[0].entries)
        for (uint8_t lv = lo; lv <= hi; ++lv) {
            const double lvx = double(lv - lo) / double(hi - lo);
            const double rate = kLaw[0] + kLaw[1] * e.axes.get("info")
                              + kLaw[2] * e.axes.get("people") + kLaw[3] * e.axes.get("danger")
                              + kLaw[4] * e.axes.get("knowledge")
                              + kLaw[5] * e.axes.get("responsibility")
                              + kLaw[6] * lvx;
            out.push_back(basket(e.slug, lv, rate, 100.0));
        }
    return out;
}

std::array<uint8_t, 32> snap(uint8_t fill) {
    std::array<uint8_t, 32> s{};
    s.fill(fill);
    return s;
}

const records::AxisFitEntry* fit_of(const records::AxisPrices& p, const std::string& kind) {
    for (const auto& f : p.fits)
        if (f.kind == kind) return &f;
    return nullptr;
}

const records::AxisGateEntry* gate_of(const records::AxisPrices& p, const std::string& axis) {
    for (const auto& g : p.gate)
        if (g.axis == axis) return &g;
    return nullptr;
}

}  // namespace

// Считалка не знает закона мира и обязана вычитать его из ставок. Это и есть
// весь смысл ИР-020: цену оси никто не назначает, она читается из сделок.
TEST(AxisPricesBuild, RecoversTheWorldsLawFromItsRates) {
    const auto cats = world_catalog();
    const auto p = build_axis_prices(world_rates(), 1.0, cats, 86'400, 86'500, snap(0x01));

    // Порядок столбцов — канонический (по имени), поэтому закон сверяется ПО
    // ИМЕНИ оси, а не по позиции: базис теперь следствие употребления, и его
    // состав может меняться вместе с данными.
    ASSERT_EQ(p.basis, (std::vector<std::string>{"danger", "info", "knowledge",
                                                 "people", "physical",
                                                 "responsibility", "level"}));
    const std::map<std::string, double> law = {
        {"physical", kLawNoConst[0]}, {"info", kLawNoConst[1]},
        {"people",   kLawNoConst[2]}, {"danger", kLawNoConst[3]},
        {"knowledge", kLawNoConst[4]}, {"responsibility", kLawNoConst[5]},
        {"level", kLawNoConst[6]}};
    const auto* f = fit_of(p, "declared");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->beta.size(), 7u);
    for (size_t i = 0; i < 7; ++i)
        EXPECT_NEAR(f->beta[i], law.at(p.basis[i]), 1e-3) << "столбец " << p.basis[i];
    EXPECT_NEAR(f->r2, 1.0, 1e-6);
    EXPECT_EQ(f->rows, 72u);
    EXPECT_DOUBLE_EQ(f->weight, 7200.0);
}

// Разряд обязан пройти экзамен, а пустышка — провалить его. Если пустышка
// проходит, экзамен в этот день ничего не различает, и это видно в записи.
TEST(AxisPricesBuild, GateAdmitsGradeAndRejectsTheJunkControl) {
    const auto p = build_axis_prices(world_rates(), 1.0, world_catalog(),
                                     86'400, 86'500, snap(0x02));
    const auto* lvl  = gate_of(p, "level");
    const auto* junk = gate_of(p, "junk");
    ASSERT_NE(lvl, nullptr);
    ASSERT_NE(junk, nullptr);
    EXPECT_TRUE(lvl->admitted);
    EXPECT_FALSE(junk->admitted);
    EXPECT_LT(lvl->loo_with, lvl->bar);       // побил планку с запасом
    EXPECT_GE(junk->loo_with, junk->bar);
    EXPECT_DOUBLE_EQ(lvl->bar, lvl->loo_base * (1.0 - kAxisGateMargin));
}

// Наблюдений не больше, чем столбцов — ридж «решит» и такую систему, вернув
// гладкий правдоподобный вектор. Запись обязана вернуться пустой, а причина —
// стоять в params.
TEST(AxisPricesBuild, RefusesInsteadOfGuessingOnThinData) {
    std::vector<records::RateEntry> few = {
        basket("prof.programmer", 3, 1.10, 10.0),
        basket("prof.cook",       3, 0.95, 10.0),
    };
    const auto p = build_axis_prices(few, 1.0, world_catalog(), 86'400, 86'500, snap(3));
    EXPECT_TRUE(p.basis.empty());
    EXPECT_TRUE(p.fits.empty());
    EXPECT_NE(p.params.find("refused=underdetermined"), std::string::npos);
    EXPECT_NE(p.params.find("obs=2"), std::string::npos);
}

// Корзина без часов — это вчерашняя ставка, перенесённая вперёд, а не сегодняшнее
// свидетельство. Она не голосует в подгонке, в которую ничего не внесла.
TEST(AxisPricesBuild, CarriedForwardBasketsDoNotVote) {
    auto rates = world_rates();
    const size_t full = rates.size();
    rates.push_back(basket("prof.welder", 4, 9.99, 0.0, 0.0));   // перенос, 0 часов
    const auto p = build_axis_prices(rates, 1.0, world_catalog(), 86'400, 86'500, snap(4));
    const auto* f = fit_of(p, "declared");
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->rows, full);                 // призрак в счёт не пошёл
    EXPECT_NEAR(f->beta[3], kLaw[3], 1e-3);   // и цену опасности не сдвинул
}

// Композиция ИР-021 × ИР-020: вес корзины — независимые часы, а не сырые. Ринг,
// уценённый при усреднении ставок, обязан говорить тише и здесь.
TEST(AxisPricesBuild, WeightIsIndependenceWeightedHours) {
    auto rates = world_rates();
    for (auto& r : rates)
        if (r.specialty == "prof.welder") r.weighted_hours = 1.0;   // почти уценён
    const auto p = build_axis_prices(rates, 1.0, world_catalog(), 86'400, 86'500, snap(5));
    const auto* f = fit_of(p, "declared");
    ASSERT_NE(f, nullptr);
    EXPECT_DOUBLE_EQ(f->weight, 7200.0 - 600.0 + 6.0);
    EXPECT_NE(p.params.find("weight=weighted_hours"), std::string::npos);
}

// Заверения практиков (ИР-019) перекрывают bootstrap каталога — и это доходит
// до цен осей: значение оси ставят те, кто делает работу.
TEST(AxisPricesBuild, AttestedAxisOverridesTheCatalogProfile) {
    const auto cats = world_catalog();
    AttestedAxes att{{{"prof.welder", "danger"}, 0.10}};   // было 0.80
    const auto plain = build_axis_design(world_rates(), 1.0, cats,
                                         axis_columns_by_use(world_catalog(), world_rates()), nullptr);
    const auto over  = build_axis_design(world_rates(), 1.0, cats,
                                         axis_columns_by_use(world_catalog(), world_rates()), &att);
    ASSERT_EQ(plain.obs.size(), over.obs.size());
    // Столбец ищем ПО ИМЕНИ: порядок канонический (алфавитный), а состав базиса
    // теперь следствие употребления и может меняться вместе с данными.
    const auto dj = std::find(plain.basis.begin(), plain.basis.end(), "danger");
    ASSERT_NE(dj, plain.basis.end());
    const size_t d = static_cast<size_t>(dj - plain.basis.begin());
    bool touched = false, others_intact = true;
    for (size_t i = 0; i < plain.obs.size(); ++i) {
        const bool welder = plain.obs[i].slug == "prof.welder";
        if (welder) { touched = true; EXPECT_DOUBLE_EQ(over.obs[i].x[d], 0.10); }
        else if (plain.obs[i].x[d] != over.obs[i].x[d]) others_intact = false;
    }
    EXPECT_TRUE(touched);
    EXPECT_TRUE(others_intact);
}

// Свидетели обязаны получить тот же вектор из тех же данных, как бы ставки ни
// пришли: суммы и скользящий контроль зависят от порядка обхода.
TEST(AxisPricesBuild, DeterministicRegardlessOfRateOrder) {
    auto a = world_rates();
    auto b = a;
    std::reverse(b.begin(), b.end());
    const auto pa = build_axis_prices(a, 1.0, world_catalog(), 86'400, 86'500, snap(6));
    const auto pb = build_axis_prices(b, 1.0, world_catalog(), 86'400, 86'500, snap(6));
    ASSERT_EQ(pa.basis, pb.basis);
    ASSERT_EQ(pa.fits.size(), pb.fits.size());
    ASSERT_EQ(pa.fits[0].beta.size(), pb.fits[0].beta.size());
    for (size_t i = 0; i < pa.fits[0].beta.size(); ++i)
        EXPECT_DOUBLE_EQ(pa.fits[0].beta[i], pb.fits[0].beta[i]);   // побитово
    EXPECT_EQ(pa.gate, pb.gate);
    EXPECT_EQ(pa.params, pb.params);
}

// Нормировка W (economy.md §2б) обязана быть снята со ставок ДО подгонки,
// иначе β меряются в чужих единицах.
TEST(AxisPricesBuild, NormalizerIsDividedOutBeforeFitting) {
    auto scaled = world_rates();
    for (auto& r : scaled) r.rate *= 2.5;
    const auto p = build_axis_prices(scaled, 2.5, world_catalog(),
                                     86'400, 86'500, snap(7));
    const auto* f = fit_of(p, "declared");
    ASSERT_NE(f, nullptr);
    const std::map<std::string, double> law = {
        {"physical", kLawNoConst[0]}, {"info", kLawNoConst[1]},
        {"people",   kLawNoConst[2]}, {"danger", kLawNoConst[3]},
        {"knowledge", kLawNoConst[4]}, {"responsibility", kLawNoConst[5]},
        {"level", kLawNoConst[6]}};
    for (size_t i = 0; i < f->beta.size(); ++i)
        EXPECT_NEAR(f->beta[i], law.at(p.basis[i]), 1e-3) << p.basis[i];
    EXPECT_NE(p.params.find("W=2.5"), std::string::npos);
}

// ── Пуллинг окна опубликованных агрегатов ────────────────────────────────────

namespace {
records::DailyAggregate day_of(int64_t date, double W,
                               std::vector<records::RateEntry> rates) {
    records::DailyAggregate d{};
    d.date  = date;
    d.W     = W;
    d.rates = std::move(rates);
    return d;
}
}  // namespace

// Ставки в агрегате СЫРЫЕ и нормируются каждая своим W дня (economy.md §2б).
// Сложить их как есть — значит сложить величины в разных единицах.
TEST(AxisPricesPool, DividesOutEachDaysNormalizerBeforePooling) {
    const std::vector<records::DailyAggregate> days = {
        day_of(86'400,     2.0, {basket("prof.cook", 3, 3.0, 100.0)}),   // 1.5 в норме
        day_of(2 * 86'400, 4.0, {basket("prof.cook", 3, 2.0, 100.0)}),   // 0.5 в норме
    };
    const auto pooled = pool_daily_rates(days, 0);
    ASSERT_EQ(pooled.size(), 1u);
    EXPECT_DOUBLE_EQ(pooled[0].rate, 1.0);            // (1.5+0.5)/2, а не (3+2)/2
    EXPECT_DOUBLE_EQ(pooled[0].hours, 200.0);
    EXPECT_DOUBLE_EQ(pooled[0].weighted_hours, 200.0);
}

// Взвешивание пуллинга — независимые часы (ИР-021), а не сырые: день, почти
// целиком ушедший в дисконт, почти не двигает объединённую ставку.
TEST(AxisPricesPool, DiscountedDaySpeaksQuietly) {
    const std::vector<records::DailyAggregate> days = {
        day_of(86'400,     1.0, {basket("prof.cook", 3, 1.0, 100.0, 100.0)}),
        day_of(2 * 86'400, 1.0, {basket("prof.cook", 3, 5.0, 100.0,   1.0)}),
    };
    const auto pooled = pool_daily_rates(days, 0);
    ASSERT_EQ(pooled.size(), 1u);
    EXPECT_NEAR(pooled[0].rate, (1.0 * 100.0 + 5.0 * 1.0) / 101.0, 1e-12);
    EXPECT_DOUBLE_EQ(pooled[0].hours, 200.0);         // труд-то был — часы сырые
    EXPECT_DOUBLE_EQ(pooled[0].weighted_hours, 101.0);
}

TEST(AxisPricesPool, WindowExcludesOlderDaysAndCarriedForwardEntries) {
    const std::vector<records::DailyAggregate> days = {
        day_of(10 * 86'400, 1.0, {basket("prof.cook", 3, 9.0, 100.0)}),   // вне окна
        day_of(20 * 86'400, 1.0, {basket("prof.cook", 3, 1.0, 100.0),
                                  basket("prof.welder", 2, 7.0, 0.0, 0.0)}),  // перенос
    };
    const auto pooled = pool_daily_rates(days, 15 * 86'400);
    ASSERT_EQ(pooled.size(), 1u);
    EXPECT_EQ(pooled[0].specialty, "prof.cook");
    EXPECT_DOUBLE_EQ(pooled[0].rate, 1.0);
}

// Окно и запас экзамена обязаны попасть в params: свидетель, не знающий
// параметров, не воспроизведёт число, а тогда публиковать его бессмысленно.
TEST(AxisPricesPool, ParametersAreRecordedInTheRecord) {
    AxisPricesParams cfg{};
    cfg.window_days = 90;
    const auto p = build_axis_prices(world_rates(), 1.0, world_catalog(),
                                     86'400, 86'500, snap(8), nullptr, cfg);
    EXPECT_NE(p.params.find("window_days=90"), std::string::npos);
    EXPECT_NE(p.params.find("margin=0.050000"), std::string::npos);
    EXPECT_NE(p.params.find("const=none"), std::string::npos);
    EXPECT_NE(p.params.find("shares=none"), std::string::npos);
}

// ── Приор по β: мнение модели о том, что ещё не торговалось ──────────────────

TEST(AxisPricesPredict, ComputesTheDotProductOverThePublishedBasis) {
    const auto cats = world_catalog();
    records::AxisPrices p{};
    p.basis = {"base", "info", "people", "danger", "level"};
    p.fits  = {{"declared", {0.60, 0.50, 0.30, 0.90, 0.40}, 1.0, 60, 6000.0}};

    // prof.welder: info 0, people 0, danger 0.80; разряд 6 → колонка 1.0.
    const auto v = axis_price_predict(p, "prof.welder", 6, cats);
    ASSERT_TRUE(v.has_value());
    EXPECT_NEAR(*v, 0.60 + 0.90 * 0.80 + 0.40 * 1.0, 1e-12);

    // Разряд 1 → колонка 0.0: протокольный размах 1..6, не размах данных.
    const auto lo = axis_price_predict(p, "prof.welder", 1, cats);
    ASSERT_TRUE(lo.has_value());
    EXPECT_NEAR(*lo, 0.60 + 0.90 * 0.80, 1e-12);
}

// Приор строится на том, о чём стороны СОШЛИСЬ. Подгонка по одной стороне несёт
// интерес этой стороны, поэтому "agreed" имеет приоритет над "declared".
TEST(AxisPricesPredict, PrefersTheAgreedFitOverASingleSidesFit) {
    const auto cats = world_catalog();
    records::AxisPrices p{};
    p.basis = {"base"};
    p.fits  = {{"declared", {1.0}, 1.0, 60, 1.0},
               {"seller",   {9.0}, 1.0, 60, 1.0},
               {"agreed",   {2.0}, 1.0, 60, 1.0}};
    const auto v = axis_price_predict(p, "prof.cook", 3, cats);
    ASSERT_TRUE(v.has_value());
    EXPECT_DOUBLE_EQ(*v, 2.0);
}

// Молчание вместо догадки: отказавшаяся запись, незнакомая деятельность и
// незнакомый столбец базиса — три случая, где приора просто нет.
TEST(AxisPricesPredict, StaysSilentInsteadOfGuessing) {
    const auto cats = world_catalog();

    records::AxisPrices refused{};
    refused.params = "v1;refused=underdetermined";
    EXPECT_FALSE(axis_price_predict(refused, "prof.cook", 3, cats).has_value());

    records::AxisPrices p{};
    p.basis = {"base", "danger"};
    p.fits  = {{"declared", {0.5, 0.5}, 1.0, 60, 1.0}};
    EXPECT_FALSE(axis_price_predict(p, "prof.unknown", 3, cats).has_value());

    records::AxisPrices future{};
    future.basis = {"base", "bravery"};       // ось, которой этот код не знает
    future.fits  = {{"declared", {0.5, 0.5}, 1.0, 60, 1.0}};
    EXPECT_FALSE(axis_price_predict(future, "prof.cook", 3, cats).has_value());
}

// Заверения практиков (ИР-019) доходят и до приора: значение оси ставят те, кто
// делает работу, а не bootstrap каталога.
TEST(AxisPricesPredict, AttestedAxisMovesThePrior) {
    const auto cats = world_catalog();
    records::AxisPrices p{};
    p.basis = {"base", "danger"};
    p.fits  = {{"declared", {0.0, 1.0}, 1.0, 60, 1.0}};

    const auto plain = axis_price_predict(p, "prof.welder", 3, cats);
    ASSERT_TRUE(plain.has_value());
    EXPECT_DOUBLE_EQ(*plain, 0.80);           // bootstrap каталога

    AttestedAxes att{{{"prof.welder", "danger"}, 0.10}};
    const auto over = axis_price_predict(p, "prof.welder", 3, cats, &att);
    ASSERT_TRUE(over.has_value());
    EXPECT_DOUBLE_EQ(*over, 0.10);
}

// ── Две стороны сделки: три подгонки вместо одного усреднённого числа ────────

namespace {
const records::AxisFitEntry* need_fit(const records::AxisPrices& p,
                                      const std::string& kind) {
    for (const auto& f : p.fits)
        if (f.kind == kind) return &f;
    return nullptr;
}
double column(const records::AxisPrices& p, const records::AxisFitEntry& f,
              const std::string& name) {
    for (size_t j = 0; j < p.basis.size(); ++j)
        if (p.basis[j] == name) return f.beta[j];
    return 0.0;
}
}  // namespace

// Продавец завышает опасность своей работы, покупатель говорит как есть. Числа
// НЕ усредняются в одно: публикуются обе подгонки и согласованная.
TEST(AxisPricesSides, PublishesBothSidesAndTheAgreedFit) {
    const auto cats = world_catalog();
    AttestedAxes seller{{{"prof.welder", "danger"}, 0.95}};   // bootstrap 0.80
    AttestedAxes buyer{};                                      // говорит как есть
    const AxisSides sides{&seller, &buyer};

    const auto p = build_axis_prices(world_rates(), 1.0, cats, 86'400, 86'500,
                                     snap(0x10), nullptr, {}, &sides);
    const auto* d = need_fit(p, "declared");
    const auto* s = need_fit(p, "seller");
    const auto* b = need_fit(p, "buyer");
    const auto* a = need_fit(p, "agreed");
    ASSERT_NE(d, nullptr); ASSERT_NE(s, nullptr);
    ASSERT_NE(b, nullptr); ASSERT_NE(a, nullptr);

    // Покупатель говорил правду — по нему закон мира восстанавливается точно.
    EXPECT_NEAR(column(p, *b, "danger"), kLaw[3], 1e-3);
    // Продавец завысил ось при той же цене → цена оси у него ниже. Завышать свой
    // профиль — значит удешевлять ось для себя же.
    EXPECT_LT(column(p, *s, "danger"), column(p, *b, "danger") - 0.01);
    // Согласованная — ближе к правде, чем сторона продавца.
    EXPECT_LT(std::abs(column(p, *a, "danger") - kLaw[3]),
              std::abs(column(p, *s, "danger") - kLaw[3]));

    EXPECT_NE(p.params.find("sides=1"), std::string::npos);
    EXPECT_NE(p.params.find("tau="), std::string::npos);
}

// Расхождение ЛОКАЛИЗУЕТ спор: оно вырастает ровно на оспариваемой оси, а
// остальные не шелохнулись. Это первый сигнал, что ось на самом деле склеена
// из двух — второй сигнал (устойчивая невязка) появился раньше.
TEST(AxisPricesSides, DisagreementPointsAtTheContestedAxis) {
    const auto cats = world_catalog();
    AttestedAxes seller{{{"prof.welder", "danger"}, 0.95}};
    AttestedAxes buyer{};
    const AxisSides sides{&seller, &buyer};

    const auto p = build_axis_prices(world_rates(), 1.0, cats, 86'400, 86'500,
                                     snap(0x11), nullptr, {}, &sides);
    ASSERT_EQ(p.disagreement.size(), p.basis.size());
    double danger = 0.0, others = 0.0;
    for (size_t j = 0; j < p.basis.size(); ++j) {
        if (p.basis[j] == "danger") danger = p.disagreement[j];
        else                        others += p.disagreement[j];
    }
    EXPECT_GT(danger, 0.0);
    EXPECT_DOUBLE_EQ(others, 0.0);           // спорили ровно об одном
    // 6 корзин сварщика из 72, разрыв 0.15, веса равны → 0.15 · 6/72.
    EXPECT_NEAR(danger, 0.15 * 6.0 / 72.0, 1e-9);
}

// Пока стороны не заговорили, запись остаётся ровно такой, какой была.
TEST(AxisPricesSides, WithoutSidesTheRecordIsUnchanged) {
    const auto p = build_axis_prices(world_rates(), 1.0, world_catalog(),
                                     86'400, 86'500, snap(0x12));
    ASSERT_EQ(p.fits.size(), 1u);
    EXPECT_EQ(p.fits[0].kind, "declared");
    EXPECT_TRUE(p.disagreement.empty());
    EXPECT_EQ(p.params.find("sides="), std::string::npos);
}

// Неположительная ставка — не осторожный приор, а сломанный. Аддитивная форма
// ничем не мешает сумме уйти в минус, как только β какого-то столбца отрицателен
// (это реальный исход: отрицательный коэффициент — сравнение с базовой
// категорией, а не отрицательный труд, records.md §11.9). Такое значение ушло бы
// в build_daily_rates стартовой ставкой корзины, и от неё считались бы все
// последующие сделки. Молчим — пусть сработает номинал.
TEST(AxisPricesPredict, RefusesToHandOutANonPositivePrior) {
    const auto cats = world_catalog();

    records::AxisPrices neg{};
    neg.basis = {"base", "danger"};
    // Опасность сварщика 0.80 при β = −4.0: 1.0 − 3.2 = −2.2.
    neg.fits  = {{"declared", {1.0, -4.0}, 1.0, 60, 1.0}};
    EXPECT_FALSE(axis_price_predict(neg, "prof.welder", 3, cats).has_value());

    records::AxisPrices zero{};
    zero.basis = {"base", "danger"};
    zero.fits  = {{"declared", {1.0, -1.25}, 1.0, 60, 1.0}};  // ровно 0.0
    EXPECT_FALSE(axis_price_predict(zero, "prof.welder", 3, cats).has_value());

    // А положительный приор по-прежнему выдаётся — защита не глушит нормальный ход.
    records::AxisPrices ok{};
    ok.basis = {"base", "danger"};
    ok.fits  = {{"declared", {1.0, 0.4}, 1.0, 60, 1.0}};
    const auto v = axis_price_predict(ok, "prof.welder", 3, cats);
    ASSERT_TRUE(v.has_value());
    EXPECT_GT(*v, 1.0);
}

// Константы нет НИКОГДА. Час, у которого все степени нули, — это час, в котором
// ничего не происходило; константа была бы платой за существование, а не за
// труд, а таких выплат в этой экономике нет (records.md §12.2).
TEST(AxisPricesBuild, NeverAddsAConstantColumn) {
    const auto d = build_axis_design(world_rates(), 1.0, world_catalog(),
                                     axis_columns_by_use(world_catalog(), world_rates()), nullptr);
    EXPECT_EQ(d.basis, axis_columns_by_use(world_catalog(), world_rates()));
    for (const auto& c : d.basis) EXPECT_NE(c, std::string(kAxisBaseColumn));
    ASSERT_FALSE(d.obs.empty());
    EXPECT_EQ(d.obs.front().x.size(), axis_columns_by_use(world_catalog(), world_rates()).size());
}

// ЧТО ПОТЕРЯНО ВМЕСТЕ С ДОЛЯМИ: тождество Σ вес·невязка = 0.
//
// Оно следовало из того, что доли давали в сумме единицу в каждой строке —
// сложив нормальные уравнения долевых столбцов, получали ровно его. С
// независимыми степенями такой суммы нет.
//
// Тонкость, ради которой тест построен на явном контрпримере, а не на учебном
// мире: свойство может ПОЧТИ держаться и без долей — если вектор из единиц
// близок к тому, что складывается из столбцов. На учебном мире так и вышло
// (расхождение 9.5e-07), и тест на нём проверял бы удачу, а не правило. Здесь
// профили подобраны так, что единицу из них не сложить: (1,0), (0,1), (0.5,0.5)
// дают её, а (0.2,0.9) — уже 1.1.
TEST(AxisPricesBuild, ZeroSumIsNoLongerAnIdentityWithoutShares) {
    auto obs = [] {
        std::vector<AxisObservation> v;
        const double prof[5][2] = {{1.0, 0.0}, {0.0, 1.0}, {0.5, 0.5},
                                   {0.2, 0.9}, {0.9, 0.15}};
        const double rate[5]    = {1.00, 1.20, 1.05, 1.60, 0.95};
        const double hours[5]   = {100.0, 100.0, 100.0, 100.0, 100.0};
        for (int i = 0; i < 5; ++i) {
            AxisObservation o{};
            o.slug   = "prof.a" + std::to_string(i);
            o.level  = 1;
            o.rate   = rate[i];
            o.weight = hours[i];
            o.x      = {prof[i][0], prof[i][1]};
            v.push_back(std::move(o));
        }
        return v;
    }();
    sort_canonically(obs);
    const auto fit = fit_wls(obs);
    ASSERT_TRUE(fit.ok);

    double sw = 0.0, resid = 0.0;
    for (size_t i = 0; i < obs.size(); ++i) {
        sw    += obs[i].weight;
        resid += obs[i].weight * (obs[i].rate - fit.pred[i]);
    }
    ASSERT_GT(sw, 0.0);
    // Замерено 3.4e-03 — на три порядка выше ридж-шума (1e-06) и на тринадцать
    // выше машинного нуля: невязки по сети НЕ гасятся.
    EXPECT_GT(std::abs(resid / sw), 1e-3)
        << "без долей гашение невязок по сети перестало быть тождеством";
}

// ── Куда упирается замена разряда осями (ИР-020) ─────────────────────────────
//
// Замер на мире, где оси описывают САМУ РАБОТУ, говорит, что разряд лишний:
// скользящий контроль 0.0194 без него против 0.0317 с ним, экзамен его
// отвергает. Но чтобы это случилось здесь, знание и ответственность обязаны
// РАЗЛИЧАТЬСЯ ВНУТРИ деятельности — час сварщика 5-го разряда должен нести
// больше знания, чем час сварщика 2-го.
//
// Сегодня они этого не могут: профиль лежит в каталоге по слагу, и заверения
// (ИР-019, ИР-020) сводятся к медиане по паре (деятельность, ось) — без разряда.
// Поэтому разряд остаётся единственным, что меняется внутри деятельности, и
// экзамен обязан его принимать. Два теста ниже фиксируют обе половины: чего не
// хватает, и что арифметика готова, как только это появится.

TEST(AxisPricesGradeReplacement, GradeSurvivesWhileProfilesAreOnlyPerActivity) {
    const auto p = build_axis_prices(world_rates(), 1.0, world_catalog(),
                                     86'400, 86'500, snap(0x21));
    const records::AxisGateEntry* level = nullptr;
    for (const auto& g : p.gate)
        if (g.axis == kAxisLevel) level = &g;
    ASSERT_NE(level, nullptr);
    EXPECT_TRUE(level->admitted)
        << "пока знание и ответственность одинаковы для всех разрядов одной "
           "деятельности, разряд несёт то, чего не несёт больше ничто";
}

TEST(AxisPricesGradeReplacement, GradeIsRejectedOnceTheProfileVariesWithin) {
    // Тот же мир, но профиль знания и ответственности растёт с разрядом — так,
    // как его описали бы стороны сделки, а не каталог.
    const auto cats = world_catalog();
    std::vector<AxisObservation> obs;
    for (const auto& e : cats[0].entries)
        for (uint8_t lv = 1; lv <= 6; ++lv) {
            const double step = grade_column(lv);
            // Мастерство — это и есть знание с ответственностью, взятые гуще.
            const double know = std::min(1.0, e.axes.get("knowledge")      + 0.45 * step);
            const double resp = std::min(1.0, e.axes.get("responsibility") + 0.35 * step);
            AxisObservation o{};
            o.slug   = e.slug;
            o.level  = lv;
            o.weight = 100.0;
            o.x      = {e.axes.get("physical"), e.axes.get("info"), e.axes.get("people"),
                        e.axes.get("danger"), know, resp};
            o.rate   = kLawNoConst[0] * e.axes.get("physical")
                     + kLawNoConst[1] * e.axes.get("info")
                     + kLawNoConst[2] * e.axes.get("people")
                     + kLawNoConst[3] * e.axes.get("danger")
                     + kLawNoConst[4] * know
                     + kLawNoConst[5] * resp;
            obs.push_back(std::move(o));
        }
    sort_canonically(obs);

    std::vector<double> level;
    level.reserve(obs.size());
    for (const auto& o : obs) level.push_back(grade_column(o.level));

    const auto gate = run_axis_gate(obs, level);
    ASSERT_TRUE(gate.ok);
    EXPECT_TRUE(gate.junk_rejected);
    EXPECT_FALSE(gate.admitted)
        << "разряд объяснён знанием и ответственностью — платить за него сверху "
           "значит платить дважды за одно и то же";
}

// ── Базис как следствие употребления, а не список в коде (ИР-022) ────────────

// Столбцов ровно столько, сколько данные способны рассудить: экзамен отказывается
// судить ниже kMinRowsPerColumn наблюдений на столбец, поэтому широкий базис на
// коротком столе — не богатая модель, а модель, которую никто не проверит.
TEST(AxisColumnsByUse, WidthFollowsTheAmountOfData) {
    const auto cats  = world_catalog();
    const auto rates = world_rates();                 // 72 корзины
    const auto all   = axis_columns_by_use(cats, rates);
    EXPECT_EQ(all.size(), 72u / kMinRowsPerColumn - 1);   // 6: седьмой под кандидата

    std::vector<records::RateEntry> few(rates.begin(), rates.begin() + 30);
    EXPECT_EQ(axis_columns_by_use(cats, few).size(), 2u);

    std::vector<records::RateEntry> tiny(rates.begin(), rates.begin() + 9);
    EXPECT_TRUE(axis_columns_by_use(cats, tiny).empty());   // судить не по чему
}

// Ранг — разброс, а не вездесущность. Ось, одинаковая у ВСЕХ, не различает
// ничего и вдобавок тайком возвращает выброшенную константу.
TEST(AxisColumnsByUse, ConstantAxisNeverEarnsAColumn) {
    auto cats = world_catalog();
    for (auto& e : cats[0].entries) {
        e.axes.values["ritual"] = 0.7;      // одинаково у всех — разброс ноль
        e.axes.values["rare"]   = 0.0;      // ни у кого
    }
    cats[0].entries.front().axes.values["rare"] = 0.9;   // ровно у одного

    const auto cols = axis_columns_by_use(cats, world_rates());
    EXPECT_EQ(std::find(cols.begin(), cols.end(), "ritual"), cols.end())
        << "постоянная ось — это константа под другим именем";
    // «rare» разбросом обладает, пусть и малым, поэтому запрета на неё нет —
    // её судьбу решает экзамен, а не отбор столбцов.
}

// Порядок столбцов канонический и от порядка ставок не зависит: свидетели
// обязаны построить один и тот же план из одних и тех же данных.
TEST(AxisColumnsByUse, DeterministicRegardlessOfInputOrder) {
    const auto cats = world_catalog();
    auto rates = world_rates();
    const auto a = axis_columns_by_use(cats, rates);
    std::reverse(rates.begin(), rates.end());
    const auto b = axis_columns_by_use(cats, rates);
    EXPECT_EQ(a, b);
    EXPECT_TRUE(std::is_sorted(a.begin(), a.end()));
}

// Приор молчит про ось, которой словарь не знает: сборка со старым каталогом
// иначе прочитала бы её как ноль у всех и врала бы систематически и молча.
TEST(AxisColumnsByUse, PriorRefusesAnAxisTheVocabularyDoesNotKnow) {
    const auto cats = world_catalog();
    records::AxisPrices p{};
    p.basis = {"danger", "bravery"};
    p.fits  = {{"declared", {0.5, 0.5}, 1.0, 60, 1.0}};
    EXPECT_FALSE(axis_price_predict(p, "prof.cook", 3, cats).has_value());

    // А ось, объявленную каталогом осей, приор принимает — даже если эта работа
    // её не несёт: ноль здесь значение, а не догадка.
    records::Catalog dict;
    dict.name = "axes";
    records::CatalogEntry brave;
    brave.slug = "bravery";
    brave.ru   = "Храбрость";
    dict.entries = {brave};
    auto with_dict = cats;
    with_dict.push_back(dict);
    EXPECT_TRUE(axis_price_predict(p, "prof.cook", 3, with_dict).has_value());
}
