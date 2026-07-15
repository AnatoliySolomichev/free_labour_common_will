#pragma once

#include "aggregator.h"

#include <map>
#include <optional>
#include <string>
#include <tuple>

namespace aggregator {

// ── Grade attestation (records.md §14.4; ИР-006 шаг 2, решение 2026-07-14) ────
//
// A claimed grade (the profile's grade: tag) is cheap talk — nothing stops
// grade:6 out of thin air, and the network rate for that level then becomes a
// dishonest default at acceptance time. So a grade is ATTESTED by accepted
// work: every Acceptance whose WorkRecord names a Grade of the worker adds one
// witness. What is counted:
//
//   - DISTINCT accepting chains, never acceptances — a second chain of your
//     own must not attest you (it still can, Sybil-cheap; the honest answer to
//     that is ИР-009's trust flow — this view only refuses the FREE self-vouch:
//     the worker's own chain is excluded);
//   - total raw hours accepted at that (specialty slug, level).
//
// Chain of custody, every hop spoof-guarded to the worker's own chain:
//   Acceptance(receiver == author) → WorkRecord(owner) → agent: Grade(owner ==
//   worker) → specialty: Specialty(owner == worker, name = catalog slug §9.1).
//
// Derived and re-checkable, like every aggregator view. The display rule from
// the discussion: «разряд 4 (заявлен)» vs «разряд 3 (заверен: 5 цепей, 112 ч)»
// — the reader sees both and decides; no global score is ever computed.

struct GradeAttestation {
    uint8_t     level  = 0;
    std::size_t chains = 0;   // distinct accepting chains (worker excluded)
    double      hours  = 0;   // Σ hours_raw of those acceptances
};

class AttestationView {
public:
    static AttestationView build(const AggregatorStorage& storage);

    // Attestation at an exact (worker, specialty slug, level).
    std::optional<GradeAttestation> at_level(const UserId&      worker,
                                             const std::string& slug,
                                             uint8_t            level) const;

    // The strongest attested level for (worker, slug): highest level with at
    // least one witness. nullopt → nothing attested, any claim is bare.
    std::optional<GradeAttestation> best(const UserId&      worker,
                                         const std::string& slug) const;

private:
    // (worker, slug, level) → attestation
    std::map<std::tuple<UserId, std::string, uint8_t>, GradeAttestation> att_;
};

} // namespace aggregator
