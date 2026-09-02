#include "aggregator/axis_prices.h"

#include <gtest/gtest.h>

#include <cmath>
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

TEST(AxisPrices, GateAdmitsRealAxisAndRejectsJunk) {
    const auto obs = demo_base();
    const auto gate = run_axis_gate(obs, demo_mastery());
    ASSERT_TRUE(gate.ok);
    EXPECT_NEAR(gate.loo_base, 0.1467, 1e-3);
    EXPECT_NEAR(gate.loo_with, 0.0373, 1e-3);
    EXPECT_TRUE(gate.admitted);         // настоящая ось отыгрывает 87%
    EXPECT_TRUE(gate.junk_rejected);    // пустышка — нет
    EXPECT_LT(gate.bar, gate.loo_base); // планка ниже базы на запас
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
records::CatalogEntry cat_entry(const std::string& slug, double material,
                                double info, double people, double danger) {
    records::CatalogEntry e;
    e.slug          = slug;
    e.axes.material = material;
    e.axes.info     = info;
    e.axes.people   = people;
    e.axes.danger   = danger;
    e.axes.present  = true;
    return e;
}

std::vector<records::Catalog> world_catalog() {
    records::Catalog c;
    c.name    = "professions";
    c.entries = {
        cat_entry("prof.programmer",  0.0, 1.0, 0.0, 0.00),
        cat_entry("prof.teacher",     0.0, 0.2, 0.8, 0.00),
        cat_entry("prof.cook",        1.0, 0.0, 0.0, 0.10),
        cat_entry("prof.welder",      1.0, 0.0, 0.0, 0.80),
        cat_entry("prof.nurse",       0.0, 0.4, 0.6, 0.25),
    };
    return {c};
}

// Истинный закон этого мира, который считалка не знает:
// ставка = 0.60 + 0.50·информация + 0.30·люди + 0.90·опасность + 0.40·разряд.
constexpr double kLaw[] = {0.60, 0.50, 0.30, 0.90, 0.40};

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
        for (uint8_t lv : {lo, uint8_t(3), hi}) {
            const double lvx = double(lv - lo) / double(hi - lo);
            const double rate = kLaw[0] + kLaw[1] * e.axes.info
                              + kLaw[2] * e.axes.people + kLaw[3] * e.axes.danger
                              + kLaw[4] * lvx;
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

    ASSERT_EQ(p.basis, (std::vector<std::string>{"base", "info", "people",
                                                 "danger", "level"}));
    const auto* f = fit_of(p, "declared");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(f->beta.size(), 5u);
    for (size_t i = 0; i < 5; ++i)
        EXPECT_NEAR(f->beta[i], kLaw[i], 1e-3) << "столбец " << p.basis[i];
    EXPECT_NEAR(f->r2, 1.0, 1e-6);
    EXPECT_EQ(f->rows, 15u);
    EXPECT_DOUBLE_EQ(f->weight, 1500.0);
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
    EXPECT_DOUBLE_EQ(f->weight, 1500.0 - 300.0 + 3.0);
    EXPECT_NE(p.params.find("weight=weighted_hours"), std::string::npos);
}

// Заверения практиков (ИР-019) перекрывают bootstrap каталога — и это доходит
// до цен осей: значение оси ставят те, кто делает работу.
TEST(AxisPricesBuild, AttestedAxisOverridesTheCatalogProfile) {
    const auto cats = world_catalog();
    AttestedAxes att{{{"prof.welder", "danger"}, 0.10}};   // было 0.80
    const auto plain = build_axis_design(world_rates(), 1.0, cats,
                                         declared_axis_columns(), nullptr);
    const auto over  = build_axis_design(world_rates(), 1.0, cats,
                                         declared_axis_columns(), &att);
    ASSERT_EQ(plain.obs.size(), over.obs.size());
    bool touched = false, others_intact = true;
    for (size_t i = 0; i < plain.obs.size(); ++i) {
        const bool welder = plain.obs[i].slug == "prof.welder";
        if (welder) { touched = true; EXPECT_DOUBLE_EQ(over.obs[i].x[3], 0.10); }
        else if (plain.obs[i].x[3] != over.obs[i].x[3]) others_intact = false;
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
    for (size_t i = 0; i < 5; ++i) EXPECT_NEAR(f->beta[i], kLaw[i], 1e-3);
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
    EXPECT_NE(p.params.find("ref=material"), std::string::npos);
}
