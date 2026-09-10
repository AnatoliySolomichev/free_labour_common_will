#pragma once

// Specialty cloud (ИР-018 → ИР-022) — which kinds of work are paid for the same
// things.
//
// Where a specialty sits is READ off its settled deals. Every deal itemizes its
// price by axis (records.md §9.5 v4), so the share of everything paid for a
// specialty that went to each axis is a fact, not a profile somebody declared:
//
//     electrician  danger 0.40 · precision 0.60
//     lineman      danger 0.50 · precision 0.50     → the electrician's neighbour
//     teacher      people 0.70 · knowledge 0.30     → nothing in common with him
//
// Neighbours are specialties whose price is made of the same things. That is the
// whole definition; there are no weights to argue about.
//
// It used to be the catalog's declared intensities `x`, overridden by
// practitioners' attestations (ИР-019). Both are gone (records.md §11.8): once a
// deal says what it paid for, a declared `x` is a second, unpaid answer to the
// same question, and the regression that was its other reader was removed
// (records.md §11.9).
//
// What that costs, said plainly: a specialty nobody has traded yet is not in the
// cloud. The rate prior never asks about one — it is consulted only for a basket
// that traded today, and every traded deal carries a breakdown — so for the prior
// nothing is lost. The map for people shows only work that actually happened.
//
// Shares, not prices per hour: the rate is a HEIGHT over the map, not one of its
// coordinates (specialty-axes.md §7). Two specialties paid for the same things at
// different rates are one point; otherwise "guess the rate from the neighbours"
// would become "guess it from those with a similar rate", and dear work would
// cluster with dear work, feeding inequality back into itself.

#include "aggregator.h"

#include <records/catalog.h>
#include <records/types.h>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aggregator {

// What a specialty's settled deals paid FOR. The shares add up to 1: the
// breakdown adds up to the labour price exactly (records.md §9.5 v4), and what is
// left of the payable is the carried means of production.
struct SpecialtyComposition {
    // axis (hash of its defining block, as in the axis ledger) → share of Σ payable
    std::map<std::array<uint8_t, 32>, double> axes;
    // Share of Σ payable that was means of production carried in (ИР-011). This is
    // what capital-intensity used to be — now just one more thing the price was
    // made of, with no weight of its own to decree.
    double   carried = 0.0;
    double   payable = 0.0;  // Σ labour + carried: the evidence behind the shares
    uint64_t deals   = 0;
};

// Per specialty, over every settled deal in the store — the same deals the axis
// ledger reads (records.md §11.11): settled exactly, not a self-deal, with a
// breakdown that adds up to labor_units. Resolved Acceptance → WorkRecord → Grade
// → Specialty and pooled over grades: the cloud is about the work, not the worker.
// Shares are Σ over the deals divided by Σ payable, the same way the average
// labour-hour is Σ units / Σ hours — a one-hour job and a month-long one do not
// get an equal say in what a specialty is paid for. Deterministic over the block
// set.
std::map<std::string, SpecialtyComposition> build_specialty_compositions(
    const AggregatorStorage& storage);

// How much two specialties are paid for the same things: Σ √(aⱼ·bⱼ) over the axes
// and the carried share (the Bhattacharyya coefficient). 1 — the same
// composition; 0 — nothing paid for in common.
//
// Why this and not a distance: over shares a distance never reaches zero, so two
// works with nothing in common would still count as "0.41 close" and pull each
// other's prior. Here nothing in common is exactly nothing. And no axis gets a
// weight: axes are records anyone can start (ИР-022), and weighting one would be
// the aggregator deciding which facet of labour matters. Danger is no exception —
// a dangerous job is far from a safe one because its PRICE has danger in it.
double composition_affinity(const SpecialtyComposition& a,
                            const SpecialtyComposition& b) noexcept;

// The signed cloud (records.md §11.7, specialty-axes.md §10): per specialty, its k
// nearest by affinity. A specialty with nothing in common with anyone has no
// neighbours at all, and the prior falls through to its parent. `catalogs`, if
// served, lends only the tree parent — the fallback, never the neighbourhood.
// Deterministic: points in slug order, ties broken by slug.
records::SpecialtyCloud build_specialty_cloud(
    const std::map<std::string, SpecialtyComposition>& comps,
    int64_t                                            date,
    int64_t                                            timestamp,
    const std::array<uint8_t, 32>&                     snapshot,
    const std::vector<std::array<uint8_t, 32>>&        sources  = {},
    unsigned                                           k        = 5,
    const std::vector<records::Catalog>*               catalogs = nullptr);

// Spectral coordinates (ИР-018 phase 3 — the picture): Laplacian eigenmaps over
// the same affinity, so near = paid for similar things, including the indirect
// similarity a neighbour list does not show. Groups with nothing in common land
// apart, as they should. Deterministic; NOT stored in the record — served on
// demand via /specialty/cloud?coords=1.
std::map<std::string, std::array<double, 2>> compute_spectral_coords(
    const std::map<std::string, SpecialtyComposition>& comps);

}  // namespace aggregator
