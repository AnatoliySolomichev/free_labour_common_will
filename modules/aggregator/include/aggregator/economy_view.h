#pragma once

#include "aggregator.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace aggregator {

// ── Economy view (records.md §13) ─────────────────────────────────────────────
//
// Derived, re-checkable data the aggregator computes by scanning every known
// block and decoding its records: idea funding boards and per-chain economic
// dossiers. Built on request — the aggregator is a cache and index, never a
// source of truth; any client can recompute every figure from the chains.

// Funding state of one idea (= one referenced record, keyed by block hash).
struct IdeaFunding {
    Hash        idea_hash{};
    std::string text;                  // Concept text when the idea block is known
    double      pledged_active  = 0;   // unsettled remainder of live pledges
    double      pledged_settled = 0;   // already paid against pledges
    std::size_t pledgers        = 0;   // distinct chains that ever pledged
    std::size_t copies          = 0;
    int64_t     reaction_sum    = 0;
};

// Economic dossier of one chain — what a counterparty looks at before
// accepting this chain's paper (records.md §12.7).
struct ChainEconomy {
    double      issued   = 0;          // own paper put into circulation
    double      redeemed = 0;          // own paper returned and annihilated
    double      received = 0;          // gross inflow (any paper)
    double      spent    = 0;          // gross outflow (any paper)
    std::size_t pledges_active  = 0;
    std::size_t pledges_settled = 0;
    std::size_t pledges_revoked = 0;
    std::size_t pledges_expired = 0;
    std::size_t works_accepted  = 0;   // acceptances referencing this chain's work
    double      labor_appraised = 0;   // total labor_units of those acceptances

    double debt() const { return issued - redeemed; }
};

class EconomyView {
public:
    // Scans all blocks in `storage`; `now` decides pledge expiry.
    static EconomyView build(const AggregatorStorage& storage, int64_t now);

    // Ideas with any economic signal, sorted by pledged_active descending.
    const std::vector<IdeaFunding>& ideas() const noexcept { return ideas_; }

    std::optional<ChainEconomy> chain(const UserId& uid) const;

private:
    std::vector<IdeaFunding>          ideas_;
    std::map<UserId, ChainEconomy>    chains_;
};

} // namespace aggregator
