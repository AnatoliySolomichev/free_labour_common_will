#pragma once

#include "aggregator.h"

#include <records/catalog.h>
#include <records/types.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace aggregator {

// Attested declared-axis values (ИР-019): per (activity, axis), the grade-weighted
// median of practitioners' AxisAttestation records — the value set by the people
// who do the work, not by decree. Below `min_attesters` the value is left out
// (preliminary) and the catalog's bootstrap value stands. Deterministic over the
// block set. Weight = the attester's Grade level in the activity (unresolved → 1).
std::map<std::pair<std::string, std::string>, double> build_axis_attestations(
    const AggregatorStorage& storage, unsigned min_attesters = 1);

// Derived axis: capital-intensity per specialty (ИР-018 phase 2). For each
// specialty (activity slug), the mean over its SETTLED accepted works of
// carried_units / (labor_units + carried_units) — how much of the appraisal is
// carried means-of-production cost (baker with oven ≈ high, teacher ≈ 0). Resolved
// Acceptance → WorkRecord → Grade → Specialty, like rates. Deterministic over the
// block set (the SpecialtyCloud snapshot must commit that set).
std::map<std::string, double> build_capital_intensity(const AggregatorStorage& storage);

// Specialty cloud (ИР-018, specialty-axes.md §10).
//
// Places every declared specialty (activity) as a point and gives it its k
// nearest neighbours + its tree parent — what a rate prior needs for thin/new
// activities. Deterministic: the same catalogs yield the same cloud, so any
// aggregator recomputes and verifies (the record commits its input via
// `snapshot`).
//
// Phase 1 (this build) uses only what is already in the catalog:
//   • distance on the declared axes (material, info, people, danger), with the
//     danger axis scaled by `danger_weight` so a dangerous activity is not a
//     neighbour of a safe one (its risk premium flows to other risky work);
//   • a substitution boost for specialties that close the same need (closed_by).
// Derived edges from live deals (co-occurrence over serials, capital-intensity
// over carry threads) are a later phase over the block store.
// `capital` (optional, ИР-018 phase 2): per-specialty capital-intensity 0..1 that
// adds a coordinate to the distance, scaled by `capital_weight` (a public param —
// how much capital-intensity pulls activities together vs the object of labour).
// Specialties absent from the map are treated as 0 (labour-like) — the prior.
records::SpecialtyCloud build_specialty_cloud(
    const std::vector<records::Catalog>&        catalogs,
    int64_t                                     date,
    int64_t                                     timestamp,
    const std::array<uint8_t, 32>&              snapshot,
    const std::vector<std::array<uint8_t, 32>>& sources        = {},
    unsigned                                    k              = 5,
    double                                      danger_weight  = 1.0,
    const std::map<std::string, double>*        capital        = nullptr,
    double                                      capital_weight = 0.0,
    // Attested declared-axis overrides (ИР-019): (activity, axis) → value; when
    // present, replaces the catalog's bootstrap value for that axis.
    const std::map<std::pair<std::string, std::string>, double>* attested = nullptr);

}  // namespace aggregator
