#include "aggregator/axis_prices.h"

#include <algorithm>
#include <cmath>

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

} // namespace aggregator
