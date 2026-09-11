#pragma once

// Work cloud (ИР-018 → ИР-022 шаг 11) — which descriptions of work are paid for
// the same things.
//
// A point is a SAMPLE: an acceptance that other deals named with `like` —
// «такая же работа, как та» (records.md §9.5). Its family is the sample and
// every settled deal that names it, directly or through another deal that does.
// A profession is no longer a line in a catalogue: it is a description of work
// people keep reusing, and the cloud shows the ones that caught on. A sample
// nobody repeated is just a deal, not a point.
//
// Where a sample sits is read off its family's settled deals: the share of
// everything paid that went to each axis, and the share that was carried means
// of production (ИР-011):
//
//     electrician  danger 0.40 · precision 0.60
//     lineman      danger 0.50 · precision 0.50     → the electrician's neighbour
//     teacher      people 0.70 · knowledge 0.30     → nothing in common with him
//
// Shares, not prices per hour: the rate is a HEIGHT over the map, not one of its
// coordinates (specialty-axes.md §7). Two descriptions of work paid for the same
// things at different rates are one point; otherwise dear work would cluster
// with dear work, feeding inequality back into itself.
//
// The cloud's machine role — a rate prior for a thin new specialty — went with
// the specialty rates: a new combination of priced axes is priced by their sum
// (records.md §11.2), and a new axis has no price until people pay for it. What
// is left is the map for people: which descriptions of work sit near which.

#include "aggregator.h"

#include <records/types.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aggregator {

using SampleKey = std::array<uint8_t, 32>;   // hash of the sample acceptance

// What a family of deals was paid FOR. The shares add up to 1: the breakdown
// adds up to the labour price exactly (records.md §9.5 v4), and what is left of
// the payable is the carried means of production.
struct WorkComposition {
    records::Ref sample;    // the sample acceptance (it may be absent from the store)
    std::string  title;     // its work's free-text title, for people
    // axis (hash of its defining block, as in the axis ledger) → share of Σ payable
    std::map<std::array<uint8_t, 32>, double> axes;
    // Share of Σ payable that was means of production carried in — what
    // capital-intensity used to be, now one more thing the price was made of.
    double   carried = 0.0;
    double   payable = 0.0;  // Σ labour + carried: the evidence behind the shares
    uint64_t deals   = 0;    // settled deals in the family
};

// A family with fewer deals than this is not a point: a sample nobody repeated
// is just a deal.
inline constexpr uint64_t kMinFamily = 2;

// Every family of at least kMinFamily settled deals, keyed by its sample. A
// deal's sample: follow `like` while it leads to a known acceptance; a deal
// without `like` is its own sample. The deals counted are the ones the axis
// ledger reads (records.md §11.11): settled exactly, not a self-deal, the work
// on the table, a breakdown that adds up. Shares are Σ over the family divided
// by Σ payable — a short job and a long one do not get an equal say.
// Deterministic over the block set.
std::map<SampleKey, WorkComposition> build_work_compositions(
    const AggregatorStorage& storage);

// How much two families are paid for the same things: Σ √(aⱼ·bⱼ) over the axes
// and the carried share (the Bhattacharyya coefficient). 1 — the same
// composition; 0 — nothing paid for in common, and then no neighbour at all.
// No axis gets a weight: axes are records anyone can start (ИР-022), and
// weighting one would be the aggregator deciding which facet of labour matters.
double composition_affinity(const WorkComposition& a,
                            const WorkComposition& b) noexcept;

// The signed cloud (records.md §11.7, specialty-axes.md §10): per sample, its k
// nearest by affinity. Deterministic: points in sample-hash order, ties broken
// by sample hash.
records::WorkCloud build_work_cloud(
    const std::map<SampleKey, WorkComposition>&  comps,
    int64_t                                      date,
    int64_t                                      timestamp,
    const std::array<uint8_t, 32>&               snapshot,
    const std::vector<std::array<uint8_t, 32>>&  sources = {},
    unsigned                                     k       = 5);

// Spectral coordinates (the picture): Laplacian eigenmaps over the same
// affinity, so near = paid for similar things, including the indirect
// similarity a neighbour list does not show. Groups with nothing in common land
// apart. Deterministic; NOT stored in the record — served on demand via
// /economy/cloud?coords=1.
std::map<SampleKey, std::array<double, 2>> compute_spectral_coords(
    const std::map<SampleKey, WorkComposition>& comps);

}  // namespace aggregator
