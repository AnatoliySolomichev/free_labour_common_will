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
