#pragma once

#include "aggregator.h"

#include <records/types.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace aggregator {

// Daily axis prices (records.md §11.2 v5, economy.md §8.2).
//
// Counts the deals of the day [day_start, day_start+86400): a Transfer carrying
// its acceptance (records.md §9.5) — settled exactly by construction, never a self-deal —
// whose work is on the table and whose breakdown adds up to its price. There is
// no specialty and no grade to resolve any more (ИР-022 шаг 11): the deal says
// what it paid for, axis by axis, and that is what is averaged.
//
// Per axis: day price = Σ units on it / Σ hours of the work that named it (an
// hour of such work counts once for every axis it named); price = alpha·day +
// (1−alpha)·previous price, or the pure day price without a previous one. Below
// min_hours of weighted volume the previous price is inherited unchanged, and an
// axis with no previous price and no evidence gets none — there is no prior to
// borrow one from, and there should not be: nobody has paid for it yet.
// Previous axes with no deals today carry forward.
//
// The default price of a work is then the sum over the axes it names
// (economy.md §2а): what the network pays for an hour of each thing the work is
// made of.

// Counterparty independence (ИР-021): how much a deal counts in the averaging.
//
// Faking HOURS is expensive (witnesses, non-overlapping slots — economy.md §3),
// but inflating k costs nothing: a pair A↔B trading both ways at an inflated
// price moves the network prices of what they name while their mutual self-issued
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
// circular AND priced above the network's price of what it named:
//
//   R      = circulating part of flow(payer→worker) / flow(payer→worker)
//   anom   = deal rate / Σ network price per hour of the axes it named
//   excess = clamp((anom − 1) / (kappa − 1), 0, 1)
//   weight = 1 − R · excess
//
// The reference is the network's own axis prices (option «б», chosen
// 2026-09-11): per axis, Σ units on it / Σ hours of the work that named it,
// over every settled deal of the window — an arithmetic mean over deals, as the
// averaged labour-hour is (ratified 2026-09-08). A deal's expected price is the
// sum over what it named, as in the rent map (records.md §11.11). An axis is
// priced only once `min_axis_payers` distinct payers have named it.
//
// What this reference can and cannot explain, measured with
// aggregator_measure_indep on the sim-year journal (grade premium there: +0.10
// per step):
//
//                                   honest volume discounted    W shift  collusion
//                                   grades 3 · 4 · 5 · 6, all            ×1.3 left
//   basket = specialty × grade      6.0 · 4.7 · 5.0 ·  3.3, 4.7%  −0.18%    8.3%
//   axes, premium not named         0.0 · 1.2 · 8.9 · 18.0, 7.5%  −1.18%    8.3%
//   axes, one axis for mastery      0.2 · 5.7 · 5.1 · 15.8, 7.1%  −0.84%    8.3%
//   axes, a distinct axis per step  7.8 · 6.2 · 6.1 ·  4.5, 6.1%  −0.46%    8.3%
//   (W in the axes rows is the v5 normalizer, read off axis prices.)
//
// That journal (2026-08-24, 1368 deals) cannot be regenerated: until 2026-09-12
// SEED fixed nothing in sim-year.sh. On the reproducible one (SEED=42, 1356
// deals) the axes rows read 7.3 / 7.2 / 6.6% of honest volume, W −1.11 / −0.80
// / −0.47%, collusion 8.3%. Four random journals spread by a point or two
// (records.md §11.2); the conclusions below hold on each.
//
// The sum explains a price by WHICH axes a deal named, never by how much it put
// on them: everybody naming an axis is compared to that axis's mean. One
// "mastery" axis shared by grades 5 and 6 leaves the 6th above it; a distinct
// axis per capability — "under 400 V" next to "under 220 V" — prices each.
// Collusion survives at 8.3% however it hides the inflation: in the common
// axes, on a mastery axis, or on an axis of its own that nobody else names.
//
// max_cycle is a dial, not a wall: a conspiracy can always use a ring one longer.
// Each extra link costs the conspirators another accomplice who can defect and
// expose them, and each increment of max_cycle costs a little honest volume
// (measured: 1.0% at 2, 2.1% at 3, 2.4% at 4, 2.5% at 5) and exponential search
// time. What longer rings still evade is left to the rent map (records.md §11.11).
//
// This is not merely empirical: collusion only pays when its price is above the
// network's price of what it names — below it the pair drags that price down. A
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
    bool    self_calibrate  = true;  // derive kappa from the network's spread of anomalies
    size_t  calib_min_deals = 8;     // fewer judgeable deals in the window → `kappa`
    // An axis's network price counts only once this many DISTINCT payers have
    // named it in the window — an axis is worth what independent people pay for
    // it (ИР-022). A deal none of whose axes has a price yet is not judged at
    // all: refusing to judge beats punishing an honest young kind of work.
    size_t  min_axis_payers = 4;
    double  kappa_min       = 1.10;  // clamps for the self-calibrated kappa
    double  kappa_max       = 1.50;
};

// The day's axis prices and the normalizer.
struct DailyPrices {
    std::vector<records::AxisPrice> axes;   // canonical: by axis hash
    // economy.md §2б: the day's average paper per labour-hour — Σ price·hours
    // over the axes / Σ hours of the day's deals (1.0 on a day without deals).
    // Each deal's hours count once per axis it named and its units are split
    // among them, so the sum adds up to what the deals paid.
    double W = 1.0;
};

// When `indep` is given, deals are folded with the independence weight above;
// `hours`/`deals` in the result stay RAW (the labour did happen — only its
// valuation is in doubt), while the price is the weighted average.
//
// The weight also governs how far the day may move a price. Re-weighting alone
// cannot fix an axis whose whole day was one colluding pair — the weight cancels
// between numerator and denominator — so the day's smoothing factor is scaled by
// the share of volume that survived weighting: a day discounted away leaves
// yesterday's price almost untouched. With `indep` absent that share is 1 and
// the smoothing is plain.
DailyPrices build_daily_prices(
    const AggregatorStorage&               storage,
    int64_t                                day_start,
    const std::vector<records::AxisPrice>& previous,
    double                                 alpha     = 0.3,
    double                                 min_hours = 0.1,
    const IndependenceParams*              indep     = nullptr);

} // namespace aggregator
