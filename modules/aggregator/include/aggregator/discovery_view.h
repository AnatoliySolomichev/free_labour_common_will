#pragma once

#include "aggregator.h"

#include <cstddef>
#include <map>
#include <set>
#include <vector>

namespace aggregator {

// ── Discovery view (sync.md §8) ───────────────────────────────────────────────
//
// Ranked merge-partner suggestions, derived from two graphs the aggregator
// already sees: economic exchange (transfers) and past merges (MERGE blocks).
// Preference order per sync.md §8: economic partners → neighbors (partners of
// partners) → high-degree hubs → the long tail as the random pool. Advisory
// only — a biased aggregator can skew suggestions, never a merge itself.

struct DiscoveryCandidate {
    UserId      chain{};
    double      econ_volume = 0;   // labor-hours exchanged with the subject
    bool        neighbor    = false;  // partner of a partner
    std::size_t merges_with = 0;   // direct merges with the subject
    std::size_t degree      = 0;   // candidate's total merge partners (hub-ness)
    double      score       = 0;
};

class DiscoveryView {
public:
    static DiscoveryView build(const AggregatorStorage& storage);

    // Known chains ranked for `uid`, best first, `limit` entries at most.
    // The subject itself is excluded; already-merged partners rank lower.
    std::vector<DiscoveryCandidate> candidates_for(const UserId& uid,
                                                   std::size_t limit) const;

private:
    std::map<UserId, std::map<UserId, double>> econ_;    // volume, both directions
    std::map<UserId, std::set<UserId>>         merges_;  // merge partners
    std::set<UserId>                           chains_;  // every seen chain
};

} // namespace aggregator
