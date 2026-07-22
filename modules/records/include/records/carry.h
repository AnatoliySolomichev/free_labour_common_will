#pragma once
// Carry thread of a means of production (ИР-011, records.md §9.4 v2): each
// use of a Tool/Material batch is a link {seq, prev, after} transferring a
// share of the UNRECOVERED cost into a work. Pure computation, mirroring
// credit.h: the caller extracts observed links from whatever blocks it holds;
// a partial view is legitimate and reported as such (`gaps`), never presented
// as the full picture.

#include "records/types.h"

#include <cstdint>
#include <vector>

namespace records {

// The recognised transfer for one use (records.md §9.4): the share of the
// unrecovered cost, forcibly zero once the asset has recovered in full.
//   carried = min(used / capacity × cost, cost − collected_before)
// capacity: Tool.life (tool-hours) or Material.qty (batch size).
double carry_step(double cost, double capacity, double used,
                  double collected_before) noexcept;

// One carry link as seen in a WorkRecord block of the owner's chain.
struct ObservedCarry {
    CarryEntry              entry;
    int64_t                 timestamp = 0;  // record's claimed timestamp
    std::array<uint8_t, 32> block_hash{};   // carrier block (equivocation evidence)
};

struct CarryHistory {
    // Total collected at the newest seen link; remainder = cost − collected
    // is the bookkeeping residual value (economy.md §5б) and the resale/
    // reissue ceiling (records.md §10.2).
    double collected = 0;

    uint64_t links_seen     = 0;      // unique seq values observed
    uint64_t links_expected = 0;      // newest seq + 1
    bool     gaps           = false;  // partial local view — honesty first

    // Objective per-link violations — checkable regardless of gaps, each one
    // alone makes the link unrecognisable:
    bool formula_mismatch = false;  // carried ≠ carry_step(...) at some link
    bool over_invariant   = false;  // after > cost at some link (money pump)
    bool after_decreasing = false;  // collected shrinks along seq

    // Set only between ADJACENT seqs actually seen (k, k+1): a break in
    // after_k + carried_{k+1} == after_{k+1} is objective there; across a gap
    // it would slander links we simply have not received.
    bool thread_inconsistent = false;

    // seq values carried by two different blocks — double-charging one asset
    // over parallel branches; objective equivocation proof.
    std::vector<uint64_t> equivocated_seqs;
};

// Digest observed links of ONE asset record (any order; the same block seen
// twice is tolerated). cost/capacity come from the Tool/Material record.
CarryHistory carry_history(std::vector<ObservedCarry> links,
                           double cost, double capacity);

} // namespace records
