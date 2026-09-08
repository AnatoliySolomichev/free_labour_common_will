#include "aggregator/axis_prices.h"

#include <algorithm>
#include <map>
#include <cmath>
#include <string>

namespace aggregator {

namespace {

// Design matrix and vectors extracted from observations, in the given order.
struct Design {
    std::vector<std::vector<double>> x;
    std::vector<double>              y;
    std::vector<double>              w;
};

Design design_of(const std::vector<AxisObservation>& obs) {
    Design d;
    d.x.reserve(obs.size());
    d.y.reserve(obs.size());
    d.w.reserve(obs.size());
    for (const auto& o : obs) {
        d.x.push_back(o.x);
        d.y.push_back(o.rate);
        d.w.push_back(o.weight);
    }
    return d;
}

double predict(const std::vector<double>& beta, const std::vector<double>& x) {
    double s = 0.0;
    for (size_t i = 0; i < beta.size() && i < x.size(); ++i) s += beta[i] * x[i];
    return s;
}

} // namespace

std::vector<double> solve_wls(const std::vector<std::vector<double>>& x,
                              const std::vector<double>&              y,
                              const std::vector<double>&              w,
                              double ridge_rel) {
    if (x.empty() || x.front().empty()) return {};
    const size_t n = x.front().size();
    // Refusing beats answering: with no more rows than columns the system is
    // under-determined, and ridge would hand back smooth plausible numbers with
    // no warning that they mean nothing.
    if (x.size() <= n) return {};

    std::vector<std::vector<double>> ata(n, std::vector<double>(n, 0.0));
    std::vector<double> atb(n, 0.0);
    for (size_t r = 0; r < x.size(); ++r) {
        for (size_t i = 0; i < n; ++i) {
            const double wi = w[r] * x[r][i];
            atb[i] += wi * y[r];
            for (size_t j = 0; j < n; ++j) ata[i][j] += wi * x[r][j];
        }
    }
    double maxdiag = 0.0;
    for (size_t i = 0; i < n; ++i) maxdiag = std::max(maxdiag, ata[i][i]);
    const double lam = ridge_rel * maxdiag;
    for (size_t i = 0; i < n; ++i) ata[i][i] += lam;

    // Gauss-Jordan with partial pivoting, mirroring the prototype exactly
    // (full elimination, then divide) so both produce the same numbers.
    std::vector<std::vector<double>> m(n, std::vector<double>(n + 1, 0.0));
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < n; ++j) m[i][j] = ata[i][j];
        m[i][n] = atb[i];
    }
    for (size_t col = 0; col < n; ++col) {
        size_t piv = col;
        for (size_t r = col + 1; r < n; ++r)
            if (std::abs(m[r][col]) > std::abs(m[piv][col])) piv = r;
        std::swap(m[col], m[piv]);
        const double d = m[col][col];
        if (d == 0.0) return {};
        for (size_t r = 0; r < n; ++r) {
            if (r == col || m[r][col] == 0.0) continue;
            const double f = m[r][col] / d;
            for (size_t c = col; c <= n; ++c) m[r][c] -= f * m[col][c];
        }
    }
    std::vector<double> beta(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        if (m[i][i] == 0.0) return {};
        beta[i] = m[i][n] / m[i][i];
    }
    return beta;
}

std::vector<double> solve_wls(const std::vector<AxisObservation>& obs) {
    const Design d = design_of(obs);
    return solve_wls(d.x, d.y, d.w);
}

AxisFit fit_wls(const std::vector<AxisObservation>& obs) {
    AxisFit out{};
    const Design d = design_of(obs);
    out.beta = solve_wls(d.x, d.y, d.w);
    if (out.beta.empty()) return out;

    double sw = 0.0, swy = 0.0;
    for (size_t i = 0; i < d.y.size(); ++i) { sw += d.w[i]; swy += d.w[i] * d.y[i]; }
    if (sw <= 0.0) { out.beta.clear(); return out; }
    const double ybar = swy / sw;

    double ss_tot = 0.0, ss_res = 0.0;
    out.pred.reserve(d.y.size());
    for (size_t i = 0; i < d.y.size(); ++i) {
        const double p = predict(out.beta, d.x[i]);
        out.pred.push_back(p);
        ss_tot += d.w[i] * (d.y[i] - ybar) * (d.y[i] - ybar);
        ss_res += d.w[i] * (d.y[i] - p) * (d.y[i] - p);
    }
    out.r2 = ss_tot > 0.0 ? 1.0 - ss_res / ss_tot : 1.0;
    out.ok = true;
    return out;
}

double loo_rmse(const std::vector<AxisObservation>& obs) {
    const Design d = design_of(obs);
    if (d.x.empty()) return -1.0;
    const size_t cols = d.x.front().size();
    // Each fold drops one row, so judging needs one more row than a plain fit.
    if (d.x.size() <= cols + 1) return -1.0;

    double se = 0.0, sw = 0.0;
    for (size_t i = 0; i < d.x.size(); ++i) {
        std::vector<std::vector<double>> x;
        std::vector<double> y, w;
        x.reserve(d.x.size() - 1); y.reserve(d.y.size() - 1); w.reserve(d.w.size() - 1);
        for (size_t j = 0; j < d.x.size(); ++j) {
            if (j == i) continue;
            x.push_back(d.x[j]); y.push_back(d.y[j]); w.push_back(d.w[j]);
        }
        const auto beta = solve_wls(x, y, w);
        if (beta.empty()) return -1.0;
        const double err = d.y[i] - predict(beta, d.x[i]);
        se += d.w[i] * err * err;
        sw += d.w[i];
    }
    if (sw <= 0.0) return -1.0;
    return std::sqrt(se / sw);
}

std::vector<double> axis_price_spread(const std::vector<AxisObservation>& obs) {
    const Design d = design_of(obs);
    if (d.x.empty()) return {};
    const size_t cols = d.x.front().size();
    if (d.x.size() <= cols + 1) return {};      // нечего оставлять за бортом

    std::vector<double> sum(cols, 0.0), sumsq(cols, 0.0);
    size_t folds = 0;
    for (size_t i = 0; i < d.x.size(); ++i) {
        std::vector<std::vector<double>> x;
        std::vector<double> y, w;
        x.reserve(d.x.size() - 1); y.reserve(d.y.size() - 1); w.reserve(d.w.size() - 1);
        for (size_t j = 0; j < d.x.size(); ++j) {
            if (j == i) continue;
            x.push_back(d.x[j]); y.push_back(d.y[j]); w.push_back(d.w[j]);
        }
        const auto beta = solve_wls(x, y, w);
        if (beta.size() != cols) return {};
        for (size_t c = 0; c < cols; ++c) { sum[c] += beta[c]; sumsq[c] += beta[c] * beta[c]; }
        ++folds;
    }
    if (folds == 0) return {};

    std::vector<double> out(cols, 0.0);
    for (size_t c = 0; c < cols; ++c) {
        const double mean = sum[c] / static_cast<double>(folds);
        out[c] = std::sqrt(std::max(0.0, sumsq[c] / static_cast<double>(folds)
                                         - mean * mean));
    }
    return out;
}

double junk_axis(const std::string& slug, uint8_t level) {
    // FNV-1a over "<slug>#<level>" — the key convention is protocol, not detail.
    const std::string key = slug + '#' + std::to_string(static_cast<int>(level));
    uint32_t h = 2166136261u;
    for (const unsigned char ch : key) {
        h ^= static_cast<uint32_t>(ch);
        h *= 16777619u;
    }
    return static_cast<double>(h % 1000u) / 1000.0;
}

AxisGate run_axis_gate(const std::vector<AxisObservation>& obs,
                       const std::vector<double>&          candidate,
                       double                              margin) {
    AxisGate g{};
    if (obs.empty() || candidate.size() != obs.size()) return g;
    // Too thin to examine anything: the sliding control is itself noisy, and on a
    // short table it hands out passes to invented columns. Refuse, admit nobody.
    if (obs.size() < (obs.front().x.size() + 1) * kMinRowsPerColumn) return g;

    auto with_column = [&obs](const std::vector<double>& col) {
        std::vector<AxisObservation> out = obs;
        for (size_t i = 0; i < out.size(); ++i) out[i].x.push_back(col[i]);
        return out;
    };
    std::vector<double> junk;
    junk.reserve(obs.size());
    for (const auto& o : obs) junk.push_back(junk_axis(o.slug, o.level));

    g.loo_base = loo_rmse(obs);
    g.loo_with = loo_rmse(with_column(candidate));
    g.loo_junk = loo_rmse(with_column(junk));
    if (g.loo_base < 0.0 || g.loo_with < 0.0 || g.loo_junk < 0.0) return g;

    g.bar           = g.loo_base * (1.0 - margin);
    g.admitted      = g.loo_with < g.bar;
    g.junk_rejected = !(g.loo_junk < g.bar);
    g.ok            = true;
    return g;
}

void sort_canonically(std::vector<AxisObservation>& obs) {
    std::sort(obs.begin(), obs.end(),
              [](const AxisObservation& a, const AxisObservation& b) {
                  return a.slug != b.slug ? a.slug < b.slug : a.level < b.level;
              });
}

// ── From a day's rates to a published price vector (records.md §11.9) ────────

namespace {

// Effective declared value of one axis: the catalog's bootstrap, overridden by
// the grade-weighted median of practitioners' attestations (ИР-019) when there
// is one. Deliberately NOT shared with cloud_view's version: the cloud weighs
// `danger` by a separate parameter and adds derived capital-intensity, which the
// design matrix must not have.
double effective_axis(const records::CatalogEntry& e, const std::string& axis,
                      const AttestedAxes* attested) {
    // Any axis name at all: the profile is a map, so there is nothing here to
    // understand or fail to understand. An axis the work does not carry reads 0
    // — "this work has none of that in it" — which is a value, not a guess.
    double v = e.axes.get(axis);
    if (attested) {
        const auto it = attested->find({e.slug, axis});
        if (it != attested->end()) v = it->second;
    }
    return v;
}

std::string fmt(double v) { return std::to_string(v); }

}  // namespace

namespace {

// Список осей в строку params: свидетель обязан знать базис, чтобы пересчитать.
std::string join_axes(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) { if (i) out += ','; out += v[i]; }
    return out;
}

} // namespace

namespace {

// Знает ли словарь такую ось: либо она объявлена записью каталога осей
// (docs/catalogs/axes.json), либо её несёт чей-нибудь профиль.
bool axis_is_known(const std::vector<records::Catalog>& catalogs,
                   const std::string& axis) {
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries) {
            if (e.slug == axis) return true;
            if (e.axes.values.count(axis)) return true;
        }
    return false;
}

} // namespace

std::vector<std::string> axis_columns_by_use(
    const std::vector<records::Catalog>&   catalogs,
    const std::vector<records::RateEntry>& rates,
    std::size_t                            reserve) {

    std::map<std::string, const records::CatalogEntry*> by_slug;
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries)
            if (e.axes.present) by_slug.emplace(e.slug, &e);

    // Ранг оси — её ВЗВЕШЕННЫЙ РАЗБРОС по наблюдениям, а не объём работ, которые
    // её несут. Ось, одинаковая у всех, не различает ничего: она не сообщает о
    // разнице между работами ровно ничего и вдобавок тайком возвращает
    // выброшенную константу — постоянный столбец это она и есть. Разброс
    // считается ТОЛЬКО по профилям, без единого взгляда на ставки: выбирать
    // столбцы по тому, что они объясняют, значит подгонять базис под ответ, и
    // экзамен допуска перестал бы что-либо значить.
    std::map<std::string, double> sum, sumsq;
    double wtot = 0.0;
    std::size_t rows = 0;
    for (const auto& r : rates) {
        const double w = r.weighted_hours > 0.0 ? r.weighted_hours : r.hours;
        if (w <= 0.0) continue;
        const auto it = by_slug.find(r.specialty);
        if (it == by_slug.end()) continue;
        ++rows;
        wtot += w;
        for (const auto& [axis, value] : it->second->axes.values) {
            sum[axis]   += w * value;
            sumsq[axis] += w * value * value;
        }
    }
    std::map<std::string, double> spread;
    if (wtot > 0.0)
        for (const auto& [axis, s1] : sum) {
            const double mean = s1 / wtot;
            // Нули профиля тоже наблюдения: ось, которую несёт одна работа из
            // ста, разбросом обладает, но крошечным — и это правда о ней.
            spread[axis] = std::max(0.0, sumsq[axis] / wtot - mean * mean);
        }

    // Столько столбцов, сколько данные способны рассудить, и ни одним больше.
    const std::size_t room = rows / kMinRowsPerColumn;
    if (room <= reserve) return {};
    const std::size_t take = room - reserve;

    std::vector<std::pair<std::string, double>> rank(spread.begin(), spread.end());
    std::sort(rank.begin(), rank.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;          // детерминированная развязка
              });
    if (rank.size() > take) rank.resize(take);

    std::vector<std::string> out;
    out.reserve(rank.size());
    for (const auto& [axis, w] : rank) { (void)w; out.push_back(axis); }
    std::sort(out.begin(), out.end());        // канонический порядок столбцов
    return out;
}

double grade_column(uint8_t level) {
    const double v = (static_cast<double>(level) - kGradeMin)
                   / static_cast<double>(kGradeMax - kGradeMin);
    return std::clamp(v, 0.0, 1.0);
}

AxisDesign build_axis_design(const std::vector<records::RateEntry>& rates,
                             double                                 W,
                             const std::vector<records::Catalog>&   catalogs,
                             const std::vector<std::string>&        columns,
                             const AttestedAxes*                    attested) {
    AxisDesign d{};
    // NEVER a constant term. An hour with every intensity at zero is an hour in
    // which nothing happened, and a constant is precisely what would pay for it:
    // a rate handed out for existing rather than for working. This economy has no
    // such thing (records.md §12.2) — hours are born only from a Transfer against
    // an accepted piece of work.
    for (const auto& c : columns) d.basis.push_back(c);

    std::map<std::string, const records::CatalogEntry*> by_slug;
    for (const auto& cat : catalogs)
        for (const auto& e : cat.entries)
            if (e.axes.present) by_slug.emplace(e.slug, &e);

    const double w_norm = W > 0.0 ? W : 1.0;
    for (const auto& r : rates) {
        // A basket with no traded hours is a rate carried forward from an
        // earlier day, not evidence about today; counting it would let a stale
        // number vote in a fit it contributed nothing to.
        const double weight = r.weighted_hours > 0.0 ? r.weighted_hours : r.hours;
        if (weight <= 0.0) continue;
        const auto it = by_slug.find(r.specialty);
        if (it == by_slug.end()) continue;      // no declared profile — not guessed at
        AxisObservation o{};
        o.slug   = r.specialty;
        o.level  = r.level;
        o.rate   = r.rate / w_norm;             // economy.md §2б: average hour = 1
        o.weight = weight;
        d.obs.push_back(std::move(o));
    }
    if (d.obs.empty()) return d;
    sort_canonically(d.obs);

    for (auto& o : d.obs) {
        o.x.clear();
        o.x.reserve(columns.size());
        for (const auto& c : columns) {
            if (c == kAxisLevel)
                o.x.push_back(grade_column(o.level));
            else if (c == kAxisJunk)
                o.x.push_back(junk_axis(o.slug, o.level));
            else
                o.x.push_back(effective_axis(*by_slug.at(o.slug), c, attested));
        }
    }
    return d;
}

std::vector<records::RateEntry> pool_daily_rates(
    const std::vector<records::DailyAggregate>& days, int64_t from_date) {
    struct Acc { double rw = 0.0, w = 0.0, h = 0.0; uint64_t deals = 0; };
    std::map<std::pair<std::string, uint8_t>, Acc> pool;
    for (const auto& d : days) {
        if (d.date < from_date) continue;
        const double w_norm = d.W > 0.0 ? d.W : 1.0;
        for (const auto& r : d.rates) {
            const double w = r.weighted_hours > 0.0 ? r.weighted_hours : r.hours;
            if (w <= 0.0) continue;              // carried forward, not evidence
            auto& a = pool[{r.specialty, r.level}];
            a.rw    += (r.rate / w_norm) * w;
            a.w     += w;
            a.h     += r.hours;
            a.deals += r.deals;
        }
    }
    std::vector<records::RateEntry> out;
    out.reserve(pool.size());
    for (const auto& [key, a] : pool) {          // std::map: canonical order
        records::RateEntry e{};
        e.specialty      = key.first;
        e.level          = key.second;
        e.rate           = a.rw / a.w;           // already in W = 1 units
        e.hours          = a.h;
        e.deals          = a.deals;
        e.weighted_hours = a.w;
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<double> axis_price_predict(
    const records::AxisPrices&           prices,
    const std::string&                   slug,
    uint8_t                              level,
    const std::vector<records::Catalog>& catalogs,
    const AttestedAxes*                  attested) {

    if (prices.basis.empty() || prices.fits.empty()) return std::nullopt;
    // The prior is built on what both sides agreed, when that exists: a fit made
    // of one side's declarations alone carries that side's interest.
    const records::AxisFitEntry* use = nullptr;
    for (const auto& f : prices.fits)
        if (f.kind == "agreed") use = &f;
    if (!use)
        for (const auto& f : prices.fits)
            if (f.kind == "declared") use = &f;
    if (!use || use->beta.size() != prices.basis.size()) return std::nullopt;

    const records::CatalogEntry* entry = nullptr;
    for (const auto& cat : catalogs)
        if (const auto* e = cat.find(slug))
            if (e->axes.present) { entry = e; break; }
    if (!entry) return std::nullopt;

    double sum = 0.0;
    for (size_t j = 0; j < prices.basis.size(); ++j) {
        const std::string& c = prices.basis[j];
        double x;
        if      (c == kAxisBaseColumn) x = 1.0;   // v1 records only
        else if (c == kAxisLevel)      x = grade_column(level);
        else {
            // Ось должна быть ИЗВЕСТНА словарю, даже если эта работа её не несёт.
            // Иначе сборка со старым каталогом молча подставит ноль там, где
            // публикатор считал β по ненулевым значениям, и приор будет
            // систематически врать — молчаливо, что хуже всего.
            if (!axis_is_known(catalogs, c)) return std::nullopt;
            x = effective_axis(*entry, c, attested);
        }
        sum += use->beta[j] * x;
    }
    // A rate of zero or less is not a cautious prior, it is a broken one: the
    // additive form has nothing stopping the sum from going negative once a
    // column's β is negative (a real outcome — see records.md §11.9 on why a
    // negative coefficient is a comparison, not negative labour). Handing that
    // to build_daily_rates would seed a basket at a non-positive rate and every
    // deal priced off it afterwards. Refuse and let the caller fall through to
    // par, which is what "no opinion" should look like.
    if (!(sum > 0.0)) return std::nullopt;
    return sum;
}

records::AxisPrices build_axis_prices(
    const std::vector<records::RateEntry>& rates,
    double                                 W,
    const std::vector<records::Catalog>&   catalogs,
    int64_t                                date,
    int64_t                                timestamp,
    const std::array<uint8_t, 32>&         snapshot,
    const AttestedAxes*                    attested,
    const AxisPricesParams&                cfg,
    const AxisSides*                       sides) {

    records::AxisPrices out{};
    out.date      = date;
    out.snapshot  = snapshot;
    out.timestamp = timestamp;

    const auto declared = axis_columns_by_use(catalogs, rates);
    const AxisDesign base = build_axis_design(rates, W, catalogs, declared, attested);

    std::string params =
        "v5;form=linear;const=none;shares=none;basis=by_use"
        ";axes=" + join_axes(declared) + ";cand=level"
        ";window_days=" + std::to_string(cfg.window_days)
      + ";margin="  + fmt(cfg.margin)
      + ";ridge="   + fmt(kRidgeRelative)
      + ";W="       + fmt(W > 0.0 ? W : 1.0)
      + ";weight=weighted_hours"
      + ";grade_range=" + std::to_string(static_cast<unsigned>(kGradeMin)) + "-"
                        + std::to_string(static_cast<unsigned>(kGradeMax));

    // Refusing is an answer. With no more baskets than columns the ridge would
    // still return a smooth plausible vector, and nothing downstream could tell
    // it apart from a measurement.
    if (base.obs.size() <= base.basis.size()) {
        out.params = params + ";refused=underdetermined;obs="
                   + std::to_string(base.obs.size());
        return out;
    }

    // The exam: does the grade earn a column of its own? The junk control keeps
    // the exam itself honest — if a meaningless column also clears the bar, the
    // criterion is not discriminating today and admits nobody.
    std::vector<double> cand;
    cand.reserve(base.obs.size());
    for (const auto& o : base.obs) cand.push_back(grade_column(o.level));
    const AxisGate g = run_axis_gate(base.obs, cand, cfg.margin);

    std::vector<std::string> columns = declared;
    if (!g.ok) {
        params += ";gate=unjudgeable";
    } else {
        out.gate.push_back({kAxisLevel, g.loo_base, g.loo_with, g.bar,
                            g.admitted && g.junk_rejected});
        out.gate.push_back({kAxisJunk,  g.loo_base, g.loo_junk, g.bar,
                            !g.junk_rejected});
        if (g.admitted && g.junk_rejected) columns.push_back(kAxisLevel);
        if (!g.junk_rejected) params += ";gate=junk_admitted";
    }

    const AxisDesign fin = columns.size() == declared.size()
                         ? base
                         : build_axis_design(rates, W, catalogs, columns, attested);
    const AxisFit fit = fit_wls(fin.obs);
    if (!fit.ok) {
        out.params = params + ";refused=underdetermined;obs="
                   + std::to_string(fin.obs.size());
        return out;
    }

    out.basis = fin.basis;
    double sw = 0.0;
    for (const auto& o : fin.obs) sw += o.weight;
    out.fits.push_back({"declared", fit.beta, fit.r2,
                        static_cast<uint64_t>(fin.obs.size()), sw});

    // ── The two sides, kept apart (ИР-020) ──────────────────────────────────
    if (sides && sides->seller && sides->buyer) {
        const AxisDesign ds = build_axis_design(rates, W, catalogs, columns, sides->seller);
        const AxisDesign db = build_axis_design(rates, W, catalogs, columns, sides->buyer);
        if (ds.obs.size() == fin.obs.size() && db.obs.size() == fin.obs.size()) {
            const size_t n = fin.obs.size(), m = fin.basis.size();

            // Per-observation distance between the two readings, and per-column
            // disagreement — the published fact that the sides differ, and where.
            std::vector<double> dist(n, 0.0);
            out.disagreement.assign(m, 0.0);
            double wsum = 0.0;
            for (size_t i = 0; i < n; ++i) {
                double d2 = 0.0;
                // С нулевого столбца: свободного члена больше нет, и колонка 0 —
                // такая же ось, как остальные (пропуск её был наследством базиса
                // с константой и молча терял расхождение по первой оси).
                for (size_t j = 0; j < m; ++j) {
                    const double gap = std::abs(ds.obs[i].x[j] - db.obs[i].x[j]);
                    d2 += gap * gap;
                    out.disagreement[j] += fin.obs[i].weight * gap;
                }
                dist[i] = std::sqrt(d2);
                wsum += fin.obs[i].weight;
            }
            if (wsum > 0.0)
                for (auto& d : out.disagreement) d /= wsum;

            // τ is the median distance itself, not a decreed constant. It has to
            // self-calibrate: measured, a fixed threshold is fine while disputes
            // are a minority but destroys the basis once they are the majority —
            // at 70% contested only 3.3 of 13 activities kept meaningful weight,
            // fewer than the design has columns. A τ that grows with the dispute
            // stops the weighting from eating the very coverage it needs.
            std::vector<double> sorted = dist;
            std::sort(sorted.begin(), sorted.end());
            const double tau = std::max(sorted[sorted.size() / 2], 1e-3);

            // "agreed": the midpoint of the two readings, each observation weighted
            // by how far apart the sides stood on it.
            AxisDesign da = ds;
            for (size_t i = 0; i < n; ++i) {
                for (size_t j = 0; j < m; ++j)      // тоже с нулевого: константы нет
                    da.obs[i].x[j] = 0.5 * (ds.obs[i].x[j] + db.obs[i].x[j]);
                da.obs[i].weight = fin.obs[i].weight
                                 * std::exp(-(dist[i] * dist[i]) / (tau * tau));
            }

            auto add_fit = [&](const char* kind, const AxisDesign& d) {
                const AxisFit f = fit_wls(d.obs);
                if (!f.ok) return;
                double w = 0.0;
                for (const auto& o : d.obs) w += o.weight;
                out.fits.push_back({kind, f.beta, f.r2,
                                    static_cast<uint64_t>(d.obs.size()), w});
            };
            add_fit("seller", ds);
            add_fit("buyer",  db);
            add_fit("agreed", da);
            params += ";sides=1;tau=" + fmt(tau);
        }
    }

    // Насколько твёрдо сеть сходится в цене каждой оси — то, что заменило
    // невязку по корзинам (records.md §11.9). Считается по тому же базису, что
    // и опубликованная подгонка.
    out.spread = axis_price_spread(fin.obs);

    out.params = params;
    return out;
}

} // namespace aggregator
