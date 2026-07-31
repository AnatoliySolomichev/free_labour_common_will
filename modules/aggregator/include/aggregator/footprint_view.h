#pragma once
// Economic footprint, layers 2–3 (ИР-010, records.md §11.6): what a chain's
// TRADING leaves behind, beyond what its own emission thread declares.
//
// Layer 1 (credit history) is computed client-side from the subject's own
// thread and needs nobody's trust — but a bot farm can fake it internally.
// Layers 2–3 need the whole graph, so the aggregator computes them and the
// client marks the figures "со слов агрегатора", re-checkable against raw
// records (ИР-010 decision 3).
//
// Nothing here is a score. Each field is a fact with its own forging price,
// stated next to it in the client output; reachability ("of those, how many
// are near YOU") is judged by the asker, not served as a verdict.

#include "aggregator.h"

#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace aggregator {

// A chain currently holding the subject's paper — it took the subject's risk
// and can only get its hours back if the subject works (records.md §12.7).
struct PaperHolder {
    UserId chain{};
    double units = 0;   // subject's paper held now (issued to them, not returned)
};

struct ChainFootprint {
    // ── Layer 2: dear signals — a farm cannot fake these cheaply ────────────
    // Holding someone's paper means betting on them; accepting their work
    // means having received something real.
    std::vector<PaperHolder> holders;            // sorted by units, descending
    double      paper_outstanding = 0;           // Σ holders' units
    std::size_t redeemers         = 0;           // distinct chains the subject redeemed for
    double      redeemed_to_others = 0;          // own paper annihilated on return
    std::size_t acceptors         = 0;           // distinct chains that accepted its work
    double      labor_outward     = 0;           // labor_units accepted by others
    std::size_t counterparties    = 0;           // distinct chains it exchanged with

    // ── Layer 3: structural screening — catches LAZY farms only ─────────────
    // Turnover inside the subject's circle (everything within two exchange
    // hops — ИР-010's reachability) vs. turnover crossing out of it. A closed
    // cluster has nowhere for value to flow. A smart farm adds sacrificial
    // outward edges, so this screens, never proves (ИР-009).
    double internal_turnover = 0;   // within two hops of the subject
    double boundary_turnover = 0;   // crossing out of that circle

    // 1.0 = nothing ever left the circle. Meaningless without turnover.
    double closedness() const noexcept {
        const double total = internal_turnover + boundary_turnover;
        return total > 0 ? internal_turnover / total : 0.0;
    }
    bool has_turnover() const noexcept {
        return internal_turnover + boundary_turnover > 0;
    }
};

class FootprintView {
public:
    // One full scan of every known block; footprints for every chain seen.
    static FootprintView build(const AggregatorStorage& storage);

    // The subject's footprint. Served ONE chain per request: the pairwise
    // "who owes whom" map is computable from raw chains but is not handed
    // over on a plate (ИР-010 decision 4).
    std::optional<ChainFootprint> chain(const UserId& uid) const;

private:
    std::map<UserId, ChainFootprint> chains_;
};

} // namespace aggregator
