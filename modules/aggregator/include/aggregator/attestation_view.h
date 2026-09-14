#pragma once

#include "aggregator.h"

#include <records/types.h>

#include <array>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace aggregator {

// ── Qualification, read off accepted work (records.md §14.4; ИР-022 шаг 11) ──
//
// There is no grade to attest any more. What a worker can do is what other
// people accepted from them, axis by axis: «по оси "работа под напряжением"
// приняли 5 цепей, 112 ч». The breakdown of every acceptance already says what
// the work was (records.md §9.5 v4), so the attestation needs no record of its
// own and no ladder — it is the history itself, read back.
//
// What is counted, per (worker, axis):
//
//   - DISTINCT accepting chains, never acceptances — and never the worker's own
//     chain: that vouch is free (a second chain of your own is still possible,
//     Sybil-cheap; the honest answer to that is ИР-009's trust flow);
//   - Σ hours_raw of the accepted works that named the axis.
//
// Spoof guards: the deal is signed by the acceptor who pays it, and the work it
// accepts sits in the chain its Ref names — you attest labour, not paperwork. A
// breakdown that does not add up to the price describes some other deal and is
// left out, as everywhere (records.md §9.5).
//
// Derived and re-checkable, like every aggregator view; no score is computed.

struct AttestedAxis {
    records::BlockPath axis;          // the AxisDef record
    std::size_t        chains = 0;    // distinct accepting chains (worker excluded)
    double             hours  = 0;    // Σ hours_raw of accepted work naming it
};

// All accepted work of a worker, whatever it named.
struct AttestedTotal {
    std::size_t chains = 0;
    double      hours  = 0;
};

class AttestationView {
public:
    static AttestationView build(const AggregatorStorage& storage);

    // Every axis the worker's accepted work named, most hours first (ties by the
    // axis hash, so two witnesses print the same list).
    std::vector<AttestedAxis> of(const UserId& worker) const;

    // One axis; nullopt → nobody accepted such work from this worker.
    std::optional<AttestedAxis> at(const UserId&                  worker,
                                   const std::array<uint8_t, 32>& axis_hash) const;

    AttestedTotal total(const UserId& worker) const;

private:
    std::map<std::pair<UserId, std::array<uint8_t, 32>>, AttestedAxis> axes_;
    std::map<UserId, AttestedTotal>                                    totals_;
};

} // namespace aggregator
