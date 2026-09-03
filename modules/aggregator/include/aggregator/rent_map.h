#pragma once

// Rent map (ИР-020) — what the axis prices could NOT explain, and what kind of
// thing that unexplained part is.
//
// The residual (fact − model) is DIAGNOSTICS, never a payment: it is computed
// after the fact, by a third party, out of the single price two people already
// agreed on. There is one price, not two.
//
// The same residual comes in two completely different flavours, and its size
// does not tell them apart:
//
//   scarcity — there are not enough electricians, so EVERYBODY pays more. A
//              market signal, and a useful one: "train electricians".
//   a channel — one payer keeps overpaying one worker. Not a signal; a pipe.
//
// What separates them is not how big the rent is but WHO PAYS IT. Measured on
// the sim-year run:
//
//   concentration over HOURS does not separate them — an injected channel scored
//   4.6 effective payers against scarcity's 13.7, while honest baskets ran as low
//   as 2.0, so no threshold exists;
//   concentration over the RENT does — the channel collapses to 1.0 payer, while
//   scarcity stays at 5.6, exactly where it stood before the price rose: when
//   everybody pays more the reference moves with them and nobody is in excess.
//
// NO COMPOSITE SCORE IS COMPUTED, deliberately. The three numbers are published
// side by side and the reader judges, the same discipline the credit history
// keeps (records.md §11.6: «глобальный балл не вычисляется»). A single
// "corruption index" would be precisely the centralized verdict this project
// exists in order not to need. And concentration alone is never evidence: a
// village with one bakery and three customers is concentrated and honest. Only
// the CONJUNCTION of a standing residual, persistence and few payers says
// anything — the same conjunction rule as ИР-021's discount.

#include "aggregator.h"
#include "axis_prices.h"

#include <records/types.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aggregator {

// One counterparty's settled trade in a basket over the window.
struct PayerFlow {
    double hours      = 0.0;   // raw hours
    double rate_hours = 0.0;   // Σ rate·hours, so rate = rate_hours / hours
};

// Everything the rent map needs about one (specialty, level) basket: who paid
// for it, and how it behaved over time.
struct BasketFlow {
    std::map<std::array<uint8_t, 32>, PayerFlow> payers;
    std::map<int64_t, PayerFlow>                 periods;  // period index → flow
};

// A period for the persistence count. Thirty days: short enough that a one-off
// emergency premium shows up as one period out of twelve, long enough that a
// basket traded weekly still has something in every period.
inline constexpr int64_t kRentPeriodDays = 30;

// Below this many distinct payers a basket is not judged at all. Refusing to
// judge beats slandering a young specialty that honestly has three customers —
// the same rule, and the same number, as ИР-021's `min_basket_edges`.
inline constexpr unsigned kRentMinPayers = 4;

// Settled deals of the window, grouped by basket and payer.
//
// The traversal is the rates view's, exactly: an Acceptance counts only if a
// Transfer settles it precisely (paid == labor + carried) and it is non-self,
// and its (specialty, level) is resolved through Acceptance → WorkRecord →
// Grade → Specialty. A deal whose provenance is not fully on the table does not
// vote here either. Deal rate = labor_units / hours_raw, divided by `W` so it
// is in the same normalized units as the model (economy.md §2б).
std::map<std::pair<std::string, uint8_t>, BasketFlow> build_basket_flows(
    const AggregatorStorage& storage, int64_t from_ts, int64_t to_ts, double W);

// One published row of the rent map.
struct RentRow {
    std::string slug;
    uint8_t     level = 0;
    double      fact  = 0.0;   // the basket's normalized rate
    double      pred  = 0.0;   // what the axis prices expected
    double      resid = 0.0;   // fact − pred, in standard hours per hour
    double      hours = 0.0;

    // ── the two facts that say what KIND of rent this is ──────────────────
    // How many periods of the window this basket stood above the model, out of
    // how many it traded in at all. A standing rent is a different animal from
    // a one-month spike.
    unsigned periods_over = 0;
    unsigned periods_seen = 0;
    // Effective number of payers OF THE EXCESS: 1/HHI over each payer's
    // hours × (their rate − model), counting only payers above the model. 1.0
    // means one payer accounts for the whole rent.
    double   rent_payers  = 0.0;
    unsigned payers       = 0;    // distinct payers in the basket
    // false: too few payers to judge (kRentMinPayers) — the row still carries
    // its residual, it just makes no claim about what kind of rent it is.
    bool     judged       = false;
};

// Assemble the map. `obs` and `pred` come from the same fit, aligned; `flows`
// from build_basket_flows over the same window. Rows follow `obs`, i.e. the
// canonical (slug, level) order.
std::vector<RentRow> build_rent_map(
    const std::vector<AxisObservation>& obs,
    const std::vector<double>&          pred,
    const std::map<std::pair<std::string, uint8_t>, BasketFlow>& flows,
    unsigned min_payers = kRentMinPayers);

} // namespace aggregator
