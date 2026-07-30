#pragma once

#include "aggregator.h"

#include <records/catalog.h>
#include <records/types.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aggregator {

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
    double                                      capital_weight = 0.0);

}  // namespace aggregator
