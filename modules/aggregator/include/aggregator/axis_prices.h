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

#include "aggregator.h"

#include <records/catalog.h>
#include <records/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
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

// ── From a day's rates to a published price vector (ИР-020, records.md §11.9) ─

// Column names are PROTOCOL KEYS, not display labels: they join to the catalog's
// `axes` object and to AxisAttestation::axis, so two witnesses build the same
// design matrix from the same words.
inline constexpr const char* kAxisBaseColumn = "base";   // the constant, x[0] = 1
inline constexpr const char* kAxisLevel      = "level";  // grade, the mastery axis
inline constexpr const char* kAxisJunk       = "junk";   // the exam's control column

// The declared axes taken from the catalog, in canonical column order.
//
// `material` is deliberately absent. material + info + people ≈ 1
// (specialty-axes.md §4.1), so one of the three MUST be the reference category:
// keep all three and the design matrix is singular by construction, the ridge
// splits their shared effect evenly, and that split looks like an answer without
// being one. Which one is dropped is recorded in AxisPrices::params.
std::vector<std::string> declared_axis_columns();

// (activity, axis) → the grade-weighted median of practitioners' attestations
// (ИР-019, cloud_view::build_axis_attestations), overriding the catalog's
// bootstrap value for that axis.
using AttestedAxes = std::map<std::pair<std::string, std::string>, double>;

// A design matrix plus the bookkeeping a witness needs to rebuild it.
struct AxisDesign {
    std::vector<std::string>     basis;   // basis[0] == kAxisBaseColumn
    std::vector<AxisObservation> obs;     // canonical order (slug, level)
    uint8_t level_min = 0;                // range the `level` column was scaled by;
    uint8_t level_max = 0;                // data-derived, so it goes into params
};

// Build the design from a day's rate table.
//
// One observation per (specialty, level) basket that actually traded: y is the
// basket's rate divided by W so the network's average labour-hour is 1
// (economy.md §2б), and the weight is the basket's INDEPENDENCE-WEIGHTED hours
// (ИР-021) — a basket that turned out to be one colluding ring speaks quietly
// here too, instead of colluding its way into the price of an axis.
//
// `columns` names the design columns beyond the constant; unknown names and
// baskets with no catalog profile are dropped rather than guessed at.
AxisDesign build_axis_design(
    const std::vector<records::RateEntry>&       rates,
    double                                       W,
    const std::vector<records::Catalog>&         catalogs,
    const std::vector<std::string>&              columns,
    const AttestedAxes*                          attested = nullptr);

// Public parameters of the computation. Every one of them lands in
// AxisPrices::params: a witness that cannot reproduce the parameters cannot
// reproduce the number, and then publishing it means nothing.
struct AxisPricesParams {
    // How far back the pooled cross-section reaches. One day is far too thin to
    // read a price surface out of — the prototype needed a year of the sim run
    // to reach 78 baskets. Same default window as ИР-021, for the same reason:
    // it must cover the period over which the roles in an economy alternate.
    int64_t window_days = 365;
    double  margin      = kAxisGateMargin;
};

// Pool a window of PUBLISHED daily aggregates into one cross-section.
//
// Each day's rates are raw and normalized by that day's own W (economy.md §2б),
// so W is divided out before pooling — otherwise days with different normalizers
// are summed in different units. A basket's pooled rate is the mean of its daily
// rates weighted by independence-weighted hours (ИР-021); `hours` stays the raw
// total, because the labour did happen — only its valuation was in doubt.
// Baskets with no weighted volume in the window are rates carried forward, not
// evidence, and do not appear.
std::vector<records::RateEntry> pool_daily_rates(
    const std::vector<records::DailyAggregate>& days,
    int64_t                                     from_date);

// Fit, examine the candidate axes, and package the result as a signed-ready
// record. `snapshot` must commit the input (catalog + block set), like
// SpecialtyCloud — the record is worth nothing if a witness cannot tell what it
// was computed over.
//
// Refusal is a legitimate outcome, not a failure: with no more observations than
// columns the record comes back with an empty basis and the reason in `params`.
// The ridge would happily "solve" that system and hand back smooth plausible
// noise.
records::AxisPrices build_axis_prices(
    const std::vector<records::RateEntry>&       rates,
    double                                       W,
    const std::vector<records::Catalog>&         catalogs,
    int64_t                                      date,
    int64_t                                      timestamp,
    const std::array<uint8_t, 32>&               snapshot,
    const AttestedAxes*                          attested = nullptr,
    const AxisPricesParams&                      params   = {});

} // namespace aggregator
