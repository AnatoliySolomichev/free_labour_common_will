#pragma once

#include <records/catalog.h>
#include <records/types.h>

#include <array>
#include <cstdint>
#include <vector>

namespace aggregator {

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
records::SpecialtyCloud build_specialty_cloud(
    const std::vector<records::Catalog>&        catalogs,
    int64_t                                     date,
    int64_t                                     timestamp,
    const std::array<uint8_t, 32>&              snapshot,
    const std::vector<std::array<uint8_t, 32>>& sources       = {},
    unsigned                                    k             = 5,
    double                                      danger_weight = 1.0);

}  // namespace aggregator
