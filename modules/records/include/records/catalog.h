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

// Declared coordinates of a specialty (ИР-018, specialty-axes.md), all of them
// INDEPENDENT INTENSITIES 0..1: "how much of this is in an hour of such work".
//
// There is no share group any more and nothing sums to anything (ИР-020,
// 2026-09-07). Shares (material + info + people ≈ 1) answered "with what does
// this work deal" and were complete by construction, which is why the model
// needed no constant term. Intensities buy that completeness differently: an
// hour with every intensity at zero is an hour in which nothing happened, so it
// is worth nothing — and a constant would be exactly a payment for existing,
// which this economy does not have (records.md §12.2: hours are born only from a
// Transfer against an accepted piece of work).
//
// The price of that: profile values become load-bearing. Under shares a wrong
// profile only misallocated value BETWEEN axes, because the total was anchored
// at 1; now it moves the LEVEL of the rate. This is deliberate — an hour of
// light, safe, unskilled work should be worth less than an hour of hard,
// dangerous, skilled work, and saying so is the point of the whole construction.
//
// `knowledge` and `responsibility` are what a GRADE was standing in for: a
// master's hour differs from a novice's precisely in these, and paying for the
// grade on top of them pays twice for the same thing (specialty-axes.md §4.2,
// specialty-axes.md §4.5). Measured: with them present the admission exam
// rejects the grade.
//
// Bootstrap values only. Practitioners overwrite them by attestation (ИР-019) —
// the value is set by whoever does the work, not by this file.
struct CatalogAxes {
    double physical       = 0.0;   // физическая нагрузка (specialty-axes.md §4.3)
    double info           = 0.0;   // работа со сведениями и символами
    double people         = 0.0;   // работа с людьми (specialty-axes.md §4.6)
    double danger         = 0.0;   // опасность (specialty-axes.md §4.5)
    double knowledge      = 0.0;   // порог входа, глубина (specialty-axes.md §4.2)
    double responsibility = 0.0;   // цена ошибки (specialty-axes.md §4.5)
    bool   present        = false; // true iff the entry declared an "axes" object
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
