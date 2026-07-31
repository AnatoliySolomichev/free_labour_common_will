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
// Full attestation picture per (activity, axis): the grade-weighted median AND how
// many distinct practitioners attested — so a consumer sees whether a value is
// well-supported or still preliminary (below the N threshold — ИР-019 A4, the same
// open N as records.md §14.8 п.11).
struct AttestationStat { double median; int attesters; };
std::map<std::pair<std::string, std::string>, AttestationStat>
build_axis_attestation_summary(const AggregatorStorage& storage);

// Attested overrides used by the cloud: the summary filtered to entries with at
// least `min_attesters` practitioners (below → preliminary, bootstrap stands).
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

// Spectral coordinates for the cloud (ИР-018, phase 3 — the map/picture). Laplacian
// eigenmaps over the same affinity as the neighbours: each specialty gets a 2D point
// where near = similar, revealing indirect similarity (friend-of-friend) the raw
// neighbour list does not. Deterministic; NOT stored in the SpecialtyCloud record
// (kept compact, specialty-axes.md §10) — served on demand via /specialty/cloud?coords=1.
std::map<std::string, std::array<double, 2>> compute_spectral_coords(
    const std::vector<records::Catalog>&        catalogs,
    double                                      danger_weight  = 1.0,
    const std::map<std::string, double>*        capital        = nullptr,
    double                                      capital_weight = 0.0,
    const std::map<std::pair<std::string, std::string>, double>* attested = nullptr);

}  // namespace aggregator
