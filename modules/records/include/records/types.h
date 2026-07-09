#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace records {

// ── Discriminator values (records.md §3) ─────────────────────────────────────

enum class RecordType : uint8_t {
    // Structural / protocol
    FraudClaim   = 0x01,
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
    // Economy
    Transfer       = 0x70,
    DailyAggregate = 0x71,
    Pledge         = 0x72,
    PledgeRevoke   = 0x73,
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

// ── Structural / protocol (records.md §3A) ───────────────────────────────────

// Accusation that a merge block's committed snapshot is cryptographically bogus.
// The proof is an opaque blob defined by the blockchain layer (a serialized
// FraudProofData); records carries it without interpreting it. Verification is
// done by blockchain::FraudProof::verify(kind, proof, merkle_root_of(target)).
struct FraudClaim {
    static constexpr RecordType TYPE = RecordType::FraudClaim;

    Ref                  target;   // accused merge block (chain + block hash)
    std::string          kind;     // "bad_sig" | "hash_mismatch"
    std::vector<uint8_t> proof;    // opaque proof blob (blockchain FraudProofData)
    std::string          reason;   // human-readable note (may be empty)
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

// ── Economy records (records.md §11) ─────────────────────────────────────────

// Named portion of labor-hours: `units` hours backed by the debt of `issuer`
// (records.md §12.7). Portions of different issuers never mix.
struct OriginQty {
    std::array<uint8_t, 32> issuer;  // debtor chain (UserId)
    double                  units;

    bool operator==(const OriginQty& o) const noexcept {
        return issuer == o.issuer && units == o.units;
    }
};

// The only way value moves (records.md §11.1). Lives in the sender's chain;
// the SPENDING branch is the branch the block is written into (its key signs
// the spend — per-branch purses, economy.md §5а). Debt stays chain-level:
// issuer == from → self-issue: a new debt/claim pair is born (§12.2);
// issuer == to   → redemption: the paper returns to its debtor and annihilates;
// otherwise      → endorsement: someone else's paper passed along.
struct Transfer {
    static constexpr RecordType TYPE = RecordType::Transfer;

    std::array<uint8_t, 32> from;      // sender chain (= owner of this record)
    std::array<uint8_t, 32> to;        // receiver chain
    uint32_t                to_node;   // receiver branch — whose purse is credited
    std::vector<OriginQty>  origins;   // named portions; total = transfer amount
    std::optional<Ref>      reason;    // Acceptance, Pledge, ProductionChain, ...
    int64_t                 timestamp; // Unix timestamp UTC
};

// Public promise to pay (part of) the cost of future work (records.md §11.3).
// Transfers nothing by itself: only settlement Transfers (reason → this
// pledge) carry weight. Revocable until settled; auto-expires.
struct Pledge {
    static constexpr RecordType TYPE = RecordType::Pledge;

    Ref                                    target;    // funded work/idea
    double                                 units;     // promised labor-hours
    std::optional<std::array<uint8_t, 32>> executor;  // specific chain, if any
    std::optional<int64_t>                 expires;   // after this — auto-revoked
    int64_t                                timestamp; // Unix timestamp UTC
};

// Revokes the unsettled remainder of an own pledge (records.md §11.4).
struct PledgeRevoke {
    static constexpr RecordType TYPE = RecordType::PledgeRevoke;

    Ref     pledge;     // own Pledge record
    int64_t timestamp;  // Unix timestamp UTC
};

// Daily rate of one (specialty name, grade level) pair (records.md §11.2).
struct RateEntry {
    std::string specialty;  // global key: the specialty name
    uint8_t     level;      // grade level 1–6
    double      rate;       // стч/hour, after smoothing
    double      hours;      // Σ hours_raw of the day's settled deals
    uint64_t    deals;      // number of settled deals counted

    bool operator==(const RateEntry& o) const noexcept {
        return specialty == o.specialty && level == o.level && rate == o.rate
            && hours == o.hours && deals == o.deals;
    }
};

// Signed daily specialty rates, written into the aggregator's own chain
// (records.md §11.2): the block author IS the aggregator. Only settled deals
// count; every figure is re-checkable against the chains.
struct DailyAggregate {
    static constexpr RecordType TYPE = RecordType::DailyAggregate;

    int64_t                date;       // UTC day start (ts − ts mod 86400)
    std::vector<RateEntry> rates;
    int64_t                timestamp;  // Unix timestamp UTC
};

// ── Record variant ────────────────────────────────────────────────────────────

using Record = std::variant<
    FraudClaim,
    Concept,
    ConceptLink,
    Composite,
    Copy,
    Reaction,
    Specialty,
    Grade,
    Worker,
    WorkRecord,
    Acceptance,
    Transfer,
    DailyAggregate,
    Pledge,
    PledgeRevoke
>;

} // namespace records
