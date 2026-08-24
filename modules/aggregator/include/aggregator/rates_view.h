#pragma once

#include "aggregator.h"

#include <records/types.h>

#include <cstdint>
#include <vector>

namespace aggregator {

// Daily specialty rates (records.md §11.2, economy.md §8.2).
//
// Counts only SETTLED deals of the day [day_start, day_start+86400): an
// Acceptance with at least one Transfer referencing it; self-deals (payer ==
// worker) are excluded. The (specialty, level) of a deal is resolved through
// Acceptance → WorkRecord → Grade → Specialty; unresolvable deals do not
// count — a deal enters the world average only when its whole provenance is
// on the table.
//
// day_avg = Σ labor_units / Σ hours_raw per (specialty, level);
// rate = alpha*day_avg + (1-alpha)*previous rate (pure day_avg without a
// previous one). Below min_hours of day volume the previous rate is inherited
// unchanged; previous entries with no deals today carry forward.
// When `cloud` is given, a thin bucket with no previous rate is seeded with a
// PRIOR from the specialty cloud (ИР-018, specialty-axes.md §10): the weighted
// average of its neighbours' rates at the same level (skipping neighbours that
// have no rate yet), else its tree parent's rate, else 1.0 (normalized par).
// Without a cloud the bucket is skipped, as before.

// Counterparty independence (ИР-021): how much a deal counts in the averaging.
//
// Faking HOURS is expensive (witnesses, non-overlapping slots — economy.md §3),
// but inflating k costs nothing: a pair A↔B trading both ways at an inflated
// price moves the network rate of their specialty while their mutual self-issued
// debts cancel out. Excluding self-deals (payer == worker) does not catch it.
//
// A pair is only the SHORTEST cycle. A ring A→B→C→A is invisible to any pairwise
// measure — every pair's flow in it is strictly one-way — and does the same
// damage (ИР-021 B5). So the mutuality term is not reciprocity but CIRCULATION:
// how much of an edge's flow comes back through a cycle of length ≤ max_cycle,
// found by greedy cycle cancelling, shortest cycles first.
//
// The discount is a CONJUNCTION, never circulation alone: honest economies
// circulate by nature (A works for B, B for C, C for A), and discounting
// circulation as such costs a quarter of the honest volume (measured on
// sim-year data — ИР-021). A deal is discounted only when its flow is both
// circular AND priced above its own basket:
//
//   R      = circulating part of flow(payer→worker) / flow(payer→worker)
//   anom   = deal rate / median rate of its (specialty, level) basket
//   excess = clamp((anom − 1) / (kappa − 1), 0, 1)
//   weight = 1 − R · excess
//
// max_cycle is a dial, not a wall: a conspiracy can always use a ring one longer.
// Each extra link costs the conspirators another accomplice who can defect and
// expose them, and each increment of max_cycle costs a little honest volume
// (measured: 1.0% at 2, 2.1% at 3, 2.4% at 4, 2.5% at 5) and exponential search
// time. What longer rings still evade is left to the rent map of ИР-020.
//
// This is not merely empirical: collusion only pays when its price is above the
// basket's standing rate — below it the pair drags its own rate down. A
// profitable pair MUST be a price anomaly, so the conjunction targets exactly
// those with a motive.
//
// The window must cover the period over which roles alternate: within a short
// window an honest pair looks one-way (R = 0), and so does a colluding pair that
// alternates months — it escapes entirely.
struct IndependenceParams {
    int64_t window_days     = 365;   // B2: rolling window; 0 disables entirely
    size_t  max_cycle       = 4;     // B5: longest ring cancelled; 2 = pairs only
    double  kappa           = 1.15;  // B1: fallback anomaly threshold
    bool    self_calibrate  = true;  // derive kappa from the basket's own spread
    size_t  calib_min_deals = 8;     // below this the basket uses `kappa`
    double  kappa_min       = 1.10;  // clamps for the self-calibrated kappa
    double  kappa_max       = 1.50;
};

// When `indep` is given, deals are folded with the independence weight above;
// `hours`/`deals` in the result stay RAW (the labour did happen — only its
// valuation is in doubt), while the rate is the weighted average.
//
// The weight also governs how far the day may move the rate. Re-weighting alone
// cannot fix a basket whose whole day was one colluding pair — the weight
// cancels between numerator and denominator — so the day's smoothing factor is
// scaled by the share of volume that survived weighting: a day discounted away
// leaves yesterday's rate almost untouched. With `indep` absent that share is
// 1 and the smoothing is exactly as before.
std::vector<records::RateEntry> build_daily_rates(
    const AggregatorStorage&              storage,
    int64_t                               day_start,
    const std::vector<records::RateEntry>& previous,
    double                                alpha     = 0.3,
    double                                min_hours = 0.1,
    const records::SpecialtyCloud*        cloud     = nullptr,
    const IndependenceParams*             indep     = nullptr);

} // namespace aggregator
