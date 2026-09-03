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
// `note` is the free-text line of the attestation that SET the median — the
// reason, in the words of the person whose value won, so a reader sees not only
// what the number is but why someone thought so (ИР-020). It is carried, never
// parsed: nothing downstream may branch on it.
struct AttestationStat {
    double      median    = 0.0;
    int         attesters = 0;
    std::string note;
};

// The two sides of a deal are kept APART, never averaged into one number (ИР-020).
// Their interests are opposite, which is exactly what makes their agreement worth
// something: a value both a buyer and a seller state is evidence, a value only one
// of them states is a position. Averaging them destroys the very distinction.
// Measured (13 activities, 104 deals, 40 runs): with 30% of sellers inflating an
// axis, the error of β recovered from sellers alone is 0.230 and from the
// agreement-weighted midpoint 0.065 — while in an honest world the split costs
// nothing at all (0.039 vs 0.036).
struct AxisAttestationSummary {
    AttestationStat all;     // one voice per attester; a deal-backed statement
                             // beats that attester's own free-standing one
    AttestationStat seller;  // written by the worker inside a SETTLED deal
    AttestationStat buyer;   // written by the payer inside a SETTLED deal
};

// `catalogs` is needed only to resolve deal-backed values, which are written as
// DELTAS to the catalog's bootstrap profile (records.md §11.8). Without it those
// statements are skipped rather than guessed at, and the result is exactly the
// pre-ИР-020 picture.
std::map<std::pair<std::string, std::string>, AxisAttestationSummary>
build_axis_attestation_summary(const AggregatorStorage& storage,
                               const std::vector<records::Catalog>* catalogs = nullptr);

// The summary split into the three (activity, axis) → value maps ИР-020 fits on.
// A side appears only where it actually spoke; elsewhere the map is silent and the
// catalog's bootstrap stands, so a fit is never fed an invented profile.
struct AxisProfileMaps {
    std::map<std::pair<std::string, std::string>, double> all, seller, buyer;
};
AxisProfileMaps split_axis_profiles(
    const std::map<std::pair<std::string, std::string>, AxisAttestationSummary>& summary,
    unsigned min_attesters = 1);

// Attested overrides used by the cloud: the summary filtered to entries with at
// least `min_attesters` practitioners (below → preliminary, bootstrap stands).
std::map<std::pair<std::string, std::string>, double> build_axis_attestations(
    const AggregatorStorage& storage, unsigned min_attesters = 1,
    const std::vector<records::Catalog>* catalogs = nullptr);

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
