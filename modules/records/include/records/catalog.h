#pragma once

#include <map>
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

// Declared axis values of a specialty (ИР-018, specialty-axes.md, ИР-022).
//
// A MAP, not a fixed set of fields: the axis vocabulary is not the aggregator's
// to decide. specialty-axes.md §4 names about forty axes and the pilot could
// hold six, so every new one needed a C++ change — the last centralized point
// left in the valuation of labour. Keys are axis slugs from the axis catalog
// (docs/catalogs/axes.json), which is itself an ordinary Catalog; ИР-022 moves
// them into chain blocks, and nothing here has to change when it does.
//
// All values are INDEPENDENT INTENSITIES 0..1 — "how much of this is in an hour
// of such work". Nothing sums to anything (shares were dropped 2026-09-07,
// records.md §11.9): an hour with every intensity at zero is an hour in which
// nothing happened, so it is worth nothing, and a constant term would be a
// payment for existing, which this economy does not have (records.md §12.2).
//
// Bootstrap values only. Practitioners overwrite them by attestation (ИР-019) —
// the value is set by whoever does the work, not by this file.
struct CatalogAxes {
    std::map<std::string, double> values;
    bool present = false;   // true iff the entry declared an "axes" object

    // Absent axis reads as 0: the work simply has none of that in it.
    double get(const std::string& axis) const noexcept {
        const auto it = values.find(axis);
        return it == values.end() ? 0.0 : it->second;
    }
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
