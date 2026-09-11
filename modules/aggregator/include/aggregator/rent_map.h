#pragma once

// Rent map (ИР-020 → ИР-022) — what a worker was paid beyond the network's own
// axis prices, and what KIND of thing that part is.
//
// The reference is not a model. Every deal itemizes its price by axis
// (records.md §9.5 v4), so the network's price of an hour of each axis is READ
// off the settled deals: per_hour[j] = Σ units on j / Σ hours of the work that
// named j. A deal's expected price is Σ per_hour[j] over the axes it named, and
// the residual is what it paid beyond that. No fitting, no basis, no profile to
// draw against. The regression this map used to stand on was removed
// (records.md §11.9) once measurement showed the ledger does its one remaining
// job as well — on the sim-year run with an injected channel and an injected
// scarcity, the effective payers of the excess came out
//
//     regression + x:   honest 13.6 · channel 1.3 · scarcity 14.0
//     axis ledger:      honest 10.7 · channel 1.1 · scarcity 14.0
//
// The same residual comes in two completely different flavours, and its size
// does not tell them apart:
//
//   scarcity — there are not enough electricians, so EVERYBODY pays more. A
//              market signal, and a useful one: "train electricians".
//   a channel — one payer keeps overpaying one worker. Not a signal; a pipe.
//
// What separates them is WHO PAYS the excess, not how big it is. NO COMPOSITE
// SCORE IS COMPUTED: residual, persistence and concentration are published side
// by side and the reader judges (records.md §11.6: «глобальный балл не
// вычисляется»). Concentration alone is never evidence — a village with one
// bakery and three customers is concentrated and honest; only the CONJUNCTION
// says anything, as in ИР-021's discount.
//
// Rows are per WORKER (ИР-022 шаг 11): there are no specialties to basket by.
// A channel is one payer overpaying one worker, so the worker is exactly where
// it shows. Scarcity of a kind of work shows twice: as the price of its axes
// rising day by day (records.md §11.2), and at each such worker as rent that
// all of their payers pay.

#include "aggregator.h"

#include <records/types.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aggregator {

// One settled deal, as the rent map sees it.
struct DealTrace {
    std::array<uint8_t, 32>                   payer{};
    double                                    hours = 0.0;
    double                                    units = 0.0;  // labour hours paid
    std::map<std::array<uint8_t, 32>, double> axes;         // axis (by hash) → units
    int64_t                                   period = 0;   // kRentPeriodDays index
};

// The window of settled deals, per worker, with the network's axis prices.
struct RentInput {
    // worker chain → the window's settled deals of that worker
    std::map<std::array<uint8_t, 32>, std::vector<DealTrace>> workers;
    // worker → the free-text title of their latest titled work in the window.
    // For people only: nothing is computed on it.
    std::map<std::array<uint8_t, 32>, std::string>            titles;
    // axis (by the hash of its defining block) → standard hours paid per hour of
    // the work that named it, across the WHOLE network — not the worker's own,
    // or a worker everybody pays more would be their own reference and scarcity
    // would vanish into it.
    std::map<std::array<uint8_t, 32>, double>                 per_hour;
};

// A period for the persistence count. Thirty days: short enough that a one-off
// emergency premium shows up as one period out of twelve, long enough that a
// worker trading weekly still has something in every period.
inline constexpr int64_t kRentPeriodDays = 30;

// Below this many distinct payers a worker is not judged at all. Refusing to
// judge beats slandering a young worker who honestly has three customers — the
// same number as ИР-021's `min_axis_payers`.
inline constexpr unsigned kRentMinPayers = 4;

// Settled deals of the window with their itemized price.
//
// A deal counts only if a Transfer settles it precisely (paid == labor +
// carried), it is not a self-deal, the work it accepts is on the table, and its
// breakdown ADDS UP to labor_units — one that does not describes some other
// deal, the same rule as the axis ledger's.
RentInput build_rent_input(const AggregatorStorage& storage,
                           int64_t from_ts, int64_t to_ts);

// One published row of the rent map.
struct RentRow {
    std::array<uint8_t, 32> worker{};
    std::string title;           // latest title of their work, for people
    double      fact     = 0.0;  // what the worker was paid per hour
    double      expected = 0.0;  // Σ network per_hour over the axes the deals named
    double      resid    = 0.0;  // fact − expected, standard hours per hour
    double      hours    = 0.0;

    // ── the two facts that say what KIND of rent this is ──────────────────
    // How many periods of the window the worker was paid above the expected
    // price, out of how many they traded in at all. A standing rent is a
    // different animal from a one-month spike.
    unsigned periods_over = 0;
    unsigned periods_seen = 0;
    // Effective number of payers OF THE EXCESS: 1/HHI over each payer's
    // Σ (units on axis − per_hour × hours), counting only what is above the
    // network price. 1.0 means one payer accounts for the whole rent.
    double   rent_payers  = 0.0;
    unsigned payers       = 0;    // distinct payers of the worker
    // false: too few payers to judge (kRentMinPayers) — the row still carries
    // its residual, it just makes no claim about what kind of rent it is.
    bool     judged       = false;
};

// Assemble the map, one row per worker in chain order.
std::vector<RentRow> build_rent_map(const RentInput& in,
                                    unsigned min_payers = kRentMinPayers);

} // namespace aggregator
