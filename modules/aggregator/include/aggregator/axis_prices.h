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
#include <optional>
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
    std::vector<double>  x;        // design row, aligned with AxisDesign::basis
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

// Below this many observations per design column the exam does not discriminate
// and REFUSES TO JUDGE, admitting nobody. Measured on the year run by subsampling
// the 78 baskets, junk columns admitted at 5 columns:
//
//   rows   10    13    20    30    40    55    78
//   junk   20%   16%   12%    6%    4%   1.7%  0.3%      (real axis: 100% throughout)
//
// The refusal costs nothing in power — the grade is admitted at every size — so
// it is free insurance. More junk CONTROLS are not a substitute: measured, three
// controls at 13 rows cut junk to 10% but dropped the real axis to 55%, five to
// 6%/36%. Below ~30 rows no number of controls is both strict and powerful; only
// data is. 10 per column puts junk at 2.5% with the real axis still at 100%.
//
// Same discipline as `min_basket_edges` in ИР-021: refusing to judge beats
// judging badly, and the exam runs again tomorrow.
inline constexpr size_t kMinRowsPerColumn = 10;

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
// The constant term of v1 records. NOT produced any more: the shares already sum
// to 1, so a column of ones duplicated them exactly and the fit had no single
// answer (records.md §11.9). Kept only so a v1 record published earlier still
// reads back. There is no such thing as an hour of work with no object, so its
// price was never observable — and it was never anybody's income either.
inline constexpr const char* kAxisBaseColumn = "base";
inline constexpr const char* kAxisLevel      = "level";  // grade, the mastery axis
inline constexpr const char* kAxisJunk       = "junk";   // the exam's control column

// The declared axes taken from the catalog, in canonical column order.
//
// Six INDEPENDENT intensities, nothing summing to anything, and NO constant term
// (ИР-020, 2026-09-07). Shares are gone: they answered "with what does this work
// deal" and were complete by construction, but that completeness is also what
// made them a duplicate of the constant. Intensities reach completeness the
// other way — an hour with every intensity at zero is an hour in which nothing
// happened, and it is worth nothing.
//
// A constant here would be a rate paid for existing rather than for working, and
// this economy has no such thing (records.md §12.2). Its size was, in any case,
// only ever a measure of how much of pay the vocabulary could not explain.
//
// The price: profile values are now LOAD-BEARING. Under shares a wrong profile
// only misallocated value between axes, because the total was anchored at 1; now
// it moves the level of the rate itself. That is deliberate — an hour of light,
// safe, unskilled work should be worth less than an hour of hard, dangerous,
// skilled work — but it puts the whole weight on attestation (ИР-019) and on the
// two sides of a deal stating the profile independently (ИР-020).
//
// What is lost with the shares: Σ w·(y − ŷ) = 0 no longer holds by construction.
// It followed from the shares summing to 1 in every row; with independent
// intensities there is no such identity, and residuals need not cancel network-wide.
std::vector<std::string> declared_axis_columns();

// (activity, axis) → the grade-weighted median of practitioners' attestations
// (ИР-019, cloud_view::build_axis_attestations), overriding the catalog's
// bootstrap value for that axis.
using AttestedAxes = std::map<std::pair<std::string, std::string>, double>;

// Grade runs 1..6 by protocol (records.md §9.2), so the `level` column is scaled
// by that fixed range and NOT by the range that happens to appear in the data.
// A data-derived range would make β_level mean something different for every
// witness — an aggregator that saw grades 2..5 and one that saw 1..6 could not
// have their vectors compared, let alone medianed.
inline constexpr uint8_t kGradeMin = 1;
inline constexpr uint8_t kGradeMax = 6;
double grade_column(uint8_t level);

// A design matrix plus the bookkeeping a witness needs to rebuild it.
struct AxisDesign {
    std::vector<std::string>     basis;   // column names, in canonical order
    std::vector<AxisObservation> obs;     // canonical order (slug, level)
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

// The model's opinion of one (activity, grade) that has not traded: Σ βj·xj over
// the published basis. Returns nothing when the record refused to fit, when the
// activity has no declared profile, or when the basis names a column this build
// cannot reconstruct — guessing would defeat the point of publishing a basis.
//
// This is a PRIOR of last resort and nothing else. It may never displace a price
// two people agreed on: the moment the model sets prices, profiles are drawn
// instead of measured and the residual stops meaning anything (Goodhart).
std::optional<double> axis_price_predict(
    const records::AxisPrices&           prices,
    const std::string&                   slug,
    uint8_t                              level,
    const std::vector<records::Catalog>& catalogs,
    const AttestedAxes*                  attested = nullptr);

// The two sides' readings of the profile, when they exist (ИР-020). Given these,
// the record carries "seller", "buyer" and "agreed" fits beside "declared", plus
// the per-column disagreement.
//
// They are NOT averaged into one profile. The sides' interests are opposite, and
// that is exactly what makes their agreement worth something: a value both a buyer
// and a seller state is evidence, a value only one states is a position. Measured
// on 13 activities × 104 deals × 40 runs, with 30% of sellers inflating an axis:
// error of β from sellers alone 0.230, from the plain midpoint 0.111, from the
// agreement-weighted midpoint 0.065 — and in an honest world all three are equal,
// so the split costs nothing. What it does NOT catch is collusion (both sides
// stating the same lie); that is ИР-021's job, and the weights compose:
// weight = independence × agreement.
struct AxisSides {
    const AttestedAxes* seller = nullptr;
    const AttestedAxes* buyer  = nullptr;
};

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
    const AxisPricesParams&                      params   = {},
    const AxisSides*                             sides    = nullptr);

} // namespace aggregator
