#include "aggregator/axis_prices.h"

#include <algorithm>
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
// is one. Deliberately NOT shared with cloud_view's version — the cloud needs
// `material` (it is a coordinate there), the design matrix must not have it
// (it is the reference category, axis_prices.h).
double effective_axis(const records::CatalogEntry& e, const std::string& axis,
                      const AttestedAxes* attested) {
    double v = 0.0;
    if      (axis == "material") v = e.axes.material;
    else if (axis == "info")     v = e.axes.info;
    else if (axis == "people")   v = e.axes.people;
    else if (axis == "danger")   v = e.axes.danger;
    else return 0.0;
    if (attested) {
        const auto it = attested->find({e.slug, axis});
        if (it != attested->end()) v = it->second;
    }
    return v;
}

std::string fmt(double v) { return std::to_string(v); }

}  // namespace

std::vector<std::string> declared_axis_columns() {
    return {"info", "people", "danger"};
}

AxisDesign build_axis_design(const std::vector<records::RateEntry>& rates,
                             double                                 W,
                             const std::vector<records::Catalog>&   catalogs,
                             const std::vector<std::string>&        columns,
                             const AttestedAxes*                    attested) {
    AxisDesign d{};
    d.basis.push_back(kAxisBaseColumn);
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

    d.level_min = d.obs.front().level;
    d.level_max = d.obs.front().level;
    for (const auto& o : d.obs) {
        d.level_min = std::min(d.level_min, o.level);
        d.level_max = std::max(d.level_max, o.level);
    }
    const double span = d.level_max > d.level_min
                      ? static_cast<double>(d.level_max - d.level_min) : 1.0;

    for (auto& o : d.obs) {
        o.x.assign(1, 1.0);                     // the constant
        for (const auto& c : columns) {
            if (c == kAxisLevel)
                o.x.push_back((o.level - d.level_min) / span);
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

records::AxisPrices build_axis_prices(
    const std::vector<records::RateEntry>& rates,
    double                                 W,
    const std::vector<records::Catalog>&   catalogs,
    int64_t                                date,
    int64_t                                timestamp,
    const std::array<uint8_t, 32>&         snapshot,
    const AttestedAxes*                    attested,
    const AxisPricesParams&                cfg) {

    records::AxisPrices out{};
    out.date      = date;
    out.snapshot  = snapshot;
    out.timestamp = timestamp;

    const auto declared = declared_axis_columns();
    const AxisDesign base = build_axis_design(rates, W, catalogs, declared, attested);

    std::string params =
        "v1;axes=info,people,danger;ref=material;cand=level"
        ";window_days=" + std::to_string(cfg.window_days)
      + ";margin="  + fmt(cfg.margin)
      + ";ridge="   + fmt(kRidgeRelative)
      + ";W="       + fmt(W > 0.0 ? W : 1.0)
      + ";weight=weighted_hours"
      + ";level_min=" + std::to_string(static_cast<unsigned>(base.level_min))
      + ";level_max=" + std::to_string(static_cast<unsigned>(base.level_max));

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
    {
        const double span = base.level_max > base.level_min
                          ? static_cast<double>(base.level_max - base.level_min) : 1.0;
        for (const auto& o : base.obs)
            cand.push_back((o.level - base.level_min) / span);
    }
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
    out.params = params;
    return out;
}

} // namespace aggregator
