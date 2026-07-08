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
std::vector<records::RateEntry> build_daily_rates(
    const AggregatorStorage&              storage,
    int64_t                               day_start,
    const std::vector<records::RateEntry>& previous,
    double                                alpha     = 0.3,
    double                                min_hours = 0.1);

} // namespace aggregator
