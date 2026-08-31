#pragma once

// Axis prices (ИР-020): hedonic decomposition of observed labour rates.
//
//   rate_i ≈ β₀ + Σ_j β_j · x_ij        x_ij = profile of activity i on axis j
//
// β₀ is the price of a plain hour, β_j how many normalized hours a full unit of
// axis j adds. NOBODY sets these numbers — they are read out of deals that have
// already happened, the way a price index reads "square metres + district +
// floor" out of flat sales without anyone decreeing the price of a district.
//
// This header is the numeric core only: no records, no storage, no network. It
// exists so the arithmetic can be tested against the Python prototype
// (docs/pilot/axis-prices.py) before anything economic depends on it.
//
// INVARIANT, load-bearing: β is an OBSERVATION, never a law. Nothing here may
// be wired into the price of a deal. The moment "price = f(profile)" holds,
// profiles start being drawn instead of measured (Goodhart), and the residual —
// which is the whole diagnostic value — disappears by construction.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aggregator {

// One observation: a (specialty, level) basket that traded. `rate` is already
// normalized by W (economy.md §2б) so the network's average labour-hour is 1;
// `weight` is the basket's volume, so busy baskets speak louder than thin ones.
struct AxisObservation {
    std::string          slug;
    uint8_t              level  = 0;
    double               rate   = 0.0;
    double               weight = 0.0;
    std::vector<double>  x;        // design row, x[0] must be the constant 1
};

// Result of one weighted least-squares fit. `beta` empty means REFUSED — the
// system was under-determined and a fit would have been ridge-smoothed noise
// dressed as an answer.
struct AxisFit {
    std::vector<double> beta;
    std::vector<double> pred;      // in-sample prediction, aligned with input
    double              r2   = 0.0;
    bool                ok   = false;
};

// Verdict of the admission exam for one candidate axis.
struct AxisGate {
    double loo_base = 0.0;         // sliding-control error without the candidate
    double loo_with = 0.0;         // with it
    double loo_junk = 0.0;         // with a meaningless column, as a control
    double bar      = 0.0;         // loo_base · (1 − margin): what it must beat
    bool   admitted = false;
    bool   junk_rejected = false;  // false here means the exam itself is broken
    bool   ok       = false;       // false: not enough data to judge at all
};

// A candidate axis must beat the base by this much, not merely differ from it.
// A strict comparison is NOT enough: measured on 78 baskets of the year run with
// 200 independent junk columns, `loo_with < loo_base` admits pure noise 16–18%
// of the time, because the sliding control is itself noisy. A 5% margin drops
// that to 0.5% while leaving real axes untouched — grade beats the base by 87%.
inline constexpr double kAxisGateMargin = 0.05;

// Ridge is numerical insurance, not statistical regularization: it keeps a
// near-singular normal-equation matrix invertible. It does NOT create the
// information needed to separate two collinear axes — it silently splits their
// shared effect, which looks like an answer without being one.
inline constexpr double kRidgeRelative = 1e-6;

// Weighted least squares through the normal equations (XᵀWX + λI)β = XᵀWy.
// Returns empty on an under-determined system (rows ≤ columns).
std::vector<double> solve_wls(const std::vector<std::vector<double>>& x,
                              const std::vector<double>&              y,
                              const std::vector<double>&              w,
                              double ridge_rel = kRidgeRelative);

std::vector<double> solve_wls(const std::vector<AxisObservation>& obs);

// Fit plus weighted R² and in-sample predictions.
AxisFit fit_wls(const std::vector<AxisObservation>& obs);

// Weighted RMSE of leave-one-out sliding control — the admission criterion.
// R² is NOT the criterion: it rises when ANY column is added, invented ones
// included, because the fit is scored on the same rows it was fitted to. This
// scores each row with a model that never saw it. Returns a negative value when
// there is too little data to leave anything out (rows − 1 ≤ columns).
double loo_rmse(const std::vector<AxisObservation>& obs);

// A deterministic meaningless column, FNV-1a over the key — the control that
// keeps the exam honest. The KEY CONVENTION is part of the protocol, not an
// implementation detail: it is "<slug>#<level>" so two witnesses draw the same
// junk. Values land in [0, 1).
double junk_axis(const std::string& slug, uint8_t level);

// Run the exam: base basis versus base + candidate, with a junk column as the
// control. `candidate` supplies one extra design value per observation, in the
// same order as `obs`.
AxisGate run_axis_gate(const std::vector<AxisObservation>& obs,
                       const std::vector<double>&          candidate,
                       double margin = kAxisGateMargin);

// Canonical ordering by (slug, level). Sums and the leave-one-out loop are
// order-dependent in floating point, so witnesses must traverse identically;
// callers need not sort, this does it for them.
void sort_canonically(std::vector<AxisObservation>& obs);

} // namespace aggregator
