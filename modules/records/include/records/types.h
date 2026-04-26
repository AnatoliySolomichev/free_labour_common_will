#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace records {

// ── Discriminator values (records.md §3) ─────────────────────────────────────

enum class RecordType : uint8_t {
    // Knowledge graph
    Concept      = 0x40,
    ConceptLink  = 0x41,
    Composite    = 0x42,
    Copy         = 0x43,
    Reaction     = 0x44,
    // Labor
    Specialty    = 0x50,
    Grade        = 0x51,
    Worker       = 0x52,
    WorkRecord   = 0x53,
    Acceptance   = 0x54,
};

// ── Cross-chain reference (records.md §4) ────────────────────────────────────

// Points to a specific block in any user's blockchain.
// chain: UserId (root public key, 32 bytes)
// hash:  BLAKE2b-256 of the referenced block
struct Ref {
    std::array<uint8_t, 32> chain{};
    std::array<uint8_t, 32> hash{};

    bool operator==(const Ref& o) const noexcept {
        return chain == o.chain && hash == o.hash;
    }
    bool operator!=(const Ref& o) const noexcept { return !(*this == o); }
};

// ── Knowledge graph (records.md §8) ──────────────────────────────────────────

struct Concept {
    static constexpr RecordType TYPE = RecordType::Concept;

    std::string              text;
    std::vector<std::string> tags;   // may be empty
};

struct ConceptLink {
    static constexpr RecordType TYPE = RecordType::ConceptLink;

    Ref         from;
    Ref         to;
    std::string kind;   // "уточняет", "противоречит", "применяет", "порождает", ...
};

struct Composite {
    static constexpr RecordType TYPE = RecordType::Composite;

    std::string       title;
    std::vector<Ref>  parts;  // Concept / ConceptLink / other Composite refs
};

// Copy of someone else's record — required before reacting to it (records.md §8.4)
struct Copy {
    static constexpr RecordType TYPE = RecordType::Copy;

    Ref source;  // original record in another chain
};

struct Reaction {
    static constexpr RecordType TYPE = RecordType::Reaction;

    Ref    target;  // Copy or own record
    int8_t value;   // -128..+127  (negative = disagree, 0 = neutral, positive = agree)
};

// ── Labor records (records.md §9) ────────────────────────────────────────────

struct Specialty {
    static constexpr RecordType TYPE = RecordType::Specialty;

    std::string name;   // "Электрик", "Столяр", "Программист", ...
};

struct Grade {
    static constexpr RecordType TYPE = RecordType::Grade;

    Ref     specialty;  // Ref to a Specialty record
    uint8_t level;      // 1–6
};

struct Worker {
    static constexpr RecordType TYPE = RecordType::Worker;

    std::array<uint8_t, 32> chain;  // UserId of the owner (= their root public key)
};

// Named resource with quantity and unit — used in WorkRecord inputs/outputs
struct ResourceQty {
    std::string resource;  // name or description of the material/product
    double      qty;
    std::string unit;      // "кг", "м³", "шт", "л", ...

    bool operator==(const ResourceQty& o) const noexcept {
        return resource == o.resource && qty == o.qty && unit == o.unit;
    }
};

struct WorkRecord {
    static constexpr RecordType TYPE = RecordType::WorkRecord;

    Ref         agent;     // Ref to a Grade (or Role) record
    std::string action;    // description of the work performed
    int64_t     start_ts;  // Unix timestamp UTC
    double      hours;     // duration in hours

    std::vector<ResourceQty> inputs;   // may be empty
    std::vector<ResourceQty> outputs;  // may be empty
};

// Acceptance = the moment labor-hours come into existence (records.md §9.5)
struct Acceptance {
    static constexpr RecordType TYPE = RecordType::Acceptance;

    Ref                     work;         // Ref to WorkRecord
    std::array<uint8_t, 32> receiver;     // UserId of the receiver (= owner of this record)
    std::string             quality;      // e.g. "пройдено", "отклонено"
    double                  hours_raw;    // raw hours from WorkRecord
    double                  labor_units;  // hours_raw * coefficient(grade) on acceptance day
    int64_t                 timestamp;    // Unix timestamp UTC
};

// ── Record variant ────────────────────────────────────────────────────────────

using Record = std::variant<
    Concept,
    ConceptLink,
    Composite,
    Copy,
    Reaction,
    Specialty,
    Grade,
    Worker,
    WorkRecord,
    Acceptance
>;

} // namespace records
