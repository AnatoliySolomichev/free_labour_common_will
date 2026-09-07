#pragma once

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace records {

// ── Catalogs of professions and needs (records.md §8.7; docs/catalogs.md) ─────
//
// Curated reference data a participant picks from instead of inventing wording —
// that is what makes self-descriptions machine-comparable. The slug (cat:<slug>)
// is the key everything joins on: a typo in it silently breaks matching, so the
// client validates against the catalog before writing.
//
// Storage today is a hybrid: JSON files in the repository, served by the
// aggregator (GET /catalog). The target is catalogs living in chain blocks, with
// spam kept out by choosing whose catalog you trust — not by moderation. Neither
// move changes the slugs, so profiles already written stay valid.

class CatalogError : public std::runtime_error {
public:
    explicit CatalogError(const std::string& msg) : std::runtime_error(msg) {}
};

// Declared cloud coordinates of a specialty (ИР-018, specialty-axes.md).
//
// Two KINDS of coordinate live here and they behave differently:
//
//   shares     — material + info + people ≈ 1: fractional membership answering
//                "with what does this work deal", complete by construction;
//   intensities— danger, knowledge, responsibility: independent 0..1 degrees
//                answering "how hard, how dangerous, how much rides on it".
//
// Knowledge and responsibility (ИР-020, specialty-axes.md §4.2, specialty-axes.md §4.5) are what a
// grade was standing in for: a grade-5 welder's hour differs from a grade-2
// welder's precisely in these, and paying for the grade ON TOP of them pays twice
// for the same thing. Measured on a world where the axes describe the work
// itself, the admission exam REJECTS grade once they are present (sliding
// control 0.0194 without it against 0.0317 with it).
//
// These are bootstrap values only. Practitioners overwrite them by attestation
// (ИР-019) — the value is set by whoever does the work, not by this file.
struct CatalogAxes {
    double material       = 0.0;
    double info           = 0.0;
    double people         = 0.0;
    double danger         = 0.0;
    double knowledge      = 0.0;
    double responsibility = 0.0;
    bool   present        = false;   // true iff the entry declared an "axes" object
};

struct CatalogEntry {
    std::string              slug;       // "prof.electrician" — stable, never renamed
    std::string              ru;         // display name
    std::string              group;      // section, for grouping in a picker
    std::vector<std::string> aliases;    // synonyms, for search only
    std::string              parent;     // tree parent slug ("" = root/none), ИР-017
    CatalogAxes              axes;       // cloud coordinates, ИР-018
    // Needs only: professions that close this need ("need.electrical" →
    // ["prof.electrician"]). Without it matching cannot be mechanised — a person
    // knows an electrician fixes wiring, a program does not. Empty = closed by no
    // profession (need.appliances is a thing, not a service).
    std::vector<std::string> closed_by;
};

struct Catalog {
    std::string               name;      // "professions" | "needs" | ...
    std::string               version;   // "2026.07"
    std::vector<CatalogEntry> entries;

    const CatalogEntry* find(const std::string& slug) const noexcept;
};

// One catalog file. Throws CatalogError.
Catalog parse_catalog(const std::string& json);

// The aggregator's bundle: {"professions": {...}, "needs": {...}}.
// Throws CatalogError.
std::vector<Catalog> parse_catalog_bundle(const std::string& json);

// Every slug across the catalogs — the set a cat: tag is validated against.
std::set<std::string> all_slugs(const std::vector<Catalog>& catalogs);

// Entries whose slug, name or alias contains `query` — case-insensitive for
// ASCII and Cyrillic ("электрик" finds "Электрик"), so a participant can find
// the slug to write. Empty query → everything.
std::vector<const CatalogEntry*> search(const std::vector<Catalog>& catalogs,
                                        const std::string&          query);

} // namespace records
