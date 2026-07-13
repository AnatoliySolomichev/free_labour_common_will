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

struct CatalogEntry {
    std::string              slug;       // "prof.electrician" — stable, never renamed
    std::string              ru;         // display name
    std::string              group;      // section, for grouping in a picker
    std::vector<std::string> aliases;    // synonyms, for search only
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

// Entries whose slug, name or alias contains `query` (case-sensitive substring),
// so a participant can find the slug to write. Empty query → everything.
std::vector<const CatalogEntry*> search(const std::vector<Catalog>& catalogs,
                                        const std::string&          query);

} // namespace records
