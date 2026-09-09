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
    // Production
    Material     = 0x60,
    Tool         = 0x61,
    // Economy
    Transfer       = 0x70,
    DailyAggregate = 0x71,
    Pledge         = 0x72,
    PledgeRevoke   = 0x73,
    Redemption     = 0x74,
    SpecialtyCloud = 0x75,
    AxisAttestation = 0x76,
    AxisPrices      = 0x77,
    DealProfile     = 0x78,
    AxisDef         = 0x79,
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

// One link of a carry thread (records.md §9.4 v2, ИР-011): the use of one
// means of production transfers a share of its UNRECOVERED cost into the
// work. One thread per Tool/Material record across the whole chain (prev is
// a Ref, it crosses branches); two links with one prev = equivocation =
// double-charging one asset over parallel branches.
struct CarryEntry {
    Ref                src;      // Tool (records.md §10.2) or Material batch (records.md §10.1)
    double             used;     // tool-hours worked / material quantity spent
    double             carried;  // labor-hours transferred into this work
    uint64_t           seq;      // link counter of the asset's carry thread
    std::optional<Ref> prev;     // previous link; absent for the first
    double             after;    // total carried over the asset's life; ≤ cost

    bool operator==(const CarryEntry& o) const noexcept {
        return src == o.src && used == o.used && carried == o.carried
            && seq == o.seq && prev == o.prev && after == o.after;
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
    std::vector<CarryEntry>  carry;    // v2: cost carried from tools/materials
};

// Acceptance = the moment labor-hours come into existence (records.md §9.5)
// v3 (economy.md §2б): the normalizer this appraisal was made against, so a
// historical оценка reads unambiguously ("10.5 стч at W=1.03") after the method
// changes. Points at the DailyAggregate used: aggregator chain + its date + W.
struct AcceptanceNorm {
    std::array<uint8_t, 32> agg;   // aggregator chain UID
    int64_t                 date;  // DailyAggregate date used
    double                  W;     // its normalizer

    bool operator==(const AcceptanceNorm& o) const noexcept {
        return agg == o.agg && date == o.date && W == o.W;
    }
};

// One line of the itemized price (ИР-022): how many of the deal's labour hours
// went to this axis. No intensity here and no share — just hours, because hours
// are what was paid.
struct AcceptanceAxis {
    std::string axis;    // axis slug; its argument lives in an AxisDef record
    double      units;   // labour hours of THIS deal's price attributed to it

    bool operator==(const AcceptanceAxis& o) const noexcept {
        return axis == o.axis && units == o.units;
    }
};

struct Acceptance {
    static constexpr RecordType TYPE = RecordType::Acceptance;

    Ref                     work;         // Ref to WorkRecord
    std::array<uint8_t, 32> receiver;     // UserId of the receiver (= owner of this record)
    std::string             quality;      // e.g. "пройдено", "отклонено"
    double                  hours_raw;    // raw hours from WorkRecord
    double                  labor_units;  // hours_raw * coefficient(grade) on acceptance day
    int64_t                 timestamp;    // Unix timestamp UTC
    // v2 (ИР-011): Σ carried of the accepted work. Payment ceiling is
    // labor_units + carried_units; rates take labor_units only (records.md §11.2).
    std::optional<double>          carried_units;
    // v3 (economy.md §2б): normalizer provenance, best-effort at accept time.
    std::optional<AcceptanceNorm>  norm;
    // v4 (ИР-022): the price, itemized by axis. Σ units MUST equal labor_units.
    //
    // This is what makes the breakdown protocol rather than commentary: it is
    // signed together with the price, in the same record, so nobody can accept a
    // deal and then remember a convenient story about it later. There is no
    // residual because there is nothing to add up beyond what is named here, and
    // nothing is inferred, so there is nothing to draw a profile against.
    //
    // Empty is still decodable — every deal written before ИР-022 has no
    // breakdown and still paid — but a deal without one does not enter the axis
    // economy at all (records.md §11.11): it is a payment nobody explained.
    std::vector<AcceptanceAxis>    axes;
};

// ── Production records (records.md §10, v2 — ИР-011) ─────────────────────────

// A batch of consumables with a carryable remainder (records.md §10.1): the batch cost
// flows into products as the quantity is spent. Field layout mirrors Tool;
// the thread capacity is the batch size qty.
struct Material {
    static constexpr RecordType TYPE = RecordType::Material;

    std::string        name;
    std::string        unit;    // "кг", "л", "кВт·ч", "шт", ...
    std::string        desc;    // may be empty
    double             cost;    // batch cost, labor-hours
    double             qty;     // batch size in unit (carry-thread capacity)
    std::string        basis;   // "paid" | "est"
    std::optional<Ref> src;     // paid: the purchase Acceptance
    std::optional<Ref> origin;  // previous record of this batch (reissue)
    std::string        note;    // est: how the estimate was made; may be empty
};

// A tool/equipment instance with cost and design life (records.md §10.2). Wear carries
// cost into products via WorkRecord.carry. Reissue (origin) covers resale,
// downward revaluation and re-entry: new cost ≤ previous remainder.
struct Tool {
    static constexpr RecordType TYPE = RecordType::Tool;

    std::string        name;
    std::string        desc;    // may be empty
    std::string        serial;  // instance id across chains; may be empty
    double             cost;    // acquisition cost, labor-hours
    double             life;    // design life, tool-hours (thread capacity)
    std::string        basis;   // "paid" | "est"
    std::optional<Ref> src;     // paid: the purchase Acceptance
    std::optional<Ref> origin;  // previous record of this instance (reissue)
    std::string        note;    // est: how the estimate was made; may be empty
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

// One link of the chain-wide emission thread (economy.md §4.3): every
// self-issue ("−") and every redemption receipt ("+") carries a sequential
// number, a reference to the previous link (whatever branch it lives in) and
// the declared debt level after the operation. Two links with one seq =
// equivocation, an objective fraud proof.
struct EmissionLink {
    uint64_t           seq        = 0;  // chain-wide link counter
    std::optional<Ref> prev;            // previous link; absent for the first
    double             debt_after = 0;  // declared debt (negative = owing)

    bool operator==(const EmissionLink& o) const noexcept {
        return seq == o.seq && prev == o.prev && debt_after == o.debt_after;
    }
};

// The only way value moves (records.md §11.1). Lives in the sender's chain;
// the SPENDING branch is the branch the block is written into (its key signs
// the spend — per-branch purses, economy.md §5а). Debt stays chain-level:
// issuer == from → self-issue: a new debt/claim pair is born (records.md §12.2);
// issuer == to   → redemption: the paper returns to its debtor and annihilates;
// otherwise      → endorsement: someone else's paper passed along.
// v3 (economy.md §4.2/economy.md §4.3): reason is mandatory for recognition (strict
// equivalence — hours move only against accepted labor); a self-issuing
// transfer must carry its emission-thread link.
//
// v4 (records.md §11.1, ИР-006): `reason` and `settles` answer two different
// questions and so must be two fields. One `reason` could not do both: records.md §12.9
// demands it point at an Acceptance ("what am I paying for"), which left a
// Pledge honestly paid off by labour marked active forever.
struct Transfer {
    static constexpr RecordType TYPE = RecordType::Transfer;

    std::array<uint8_t, 32> from;      // sender chain (= owner of this record)
    std::array<uint8_t, 32> to;        // receiver chain
    uint32_t                to_node;   // receiver branch — whose purse is credited
    std::vector<OriginQty>  origins;   // named portions; total = transfer amount
    std::optional<Ref>      reason;    // WHAT for: Acceptance (mandatory, records.md §12.9)
    std::optional<Ref>      settles;   // WHICH promise this closes: Pledge (v4)
    int64_t                 timestamp; // Unix timestamp UTC
    // Present iff origins contain a self-issued portion (issuer == from).
    std::optional<EmissionLink> emission;
};

// Issuer's receipt for own paper returned by a payer's Transfer — the "+" link
// of the emission thread (records.md §11.5, economy.md §4.3): the debt shrinks
// by `units`. Written by the issuer upon receiving own paper; a chain's credit
// history (max closed debt, repayment speed) reads straight off the thread.
struct Redemption {
    static constexpr RecordType TYPE = RecordType::Redemption;

    Ref          transfer;   // the payer's Transfer that returned the paper
    double       units;      // own paper annihilated
    EmissionLink link;       // mandatory: every redemption is threaded
    int64_t      timestamp;  // Unix timestamp UTC
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
    double      hours;      // Σ hours_raw of the day's settled deals, RAW
    uint64_t    deals;      // number of settled deals counted
    // v2 (ИР-021): Σ weight·hours_raw — the volume that survived counterparty
    // independence weighting. credibility = weighted_hours / hours ∈ [0,1] says
    // how much of this basket's day was trusted evidence, so a reader sees not
    // only the rate but how much was discounted to get it. Old (5-field)
    // entries decode with weighted_hours = hours: nothing was discounted.
    double      weighted_hours = 0.0;

    bool operator==(const RateEntry& o) const noexcept {
        return specialty == o.specialty && level == o.level && rate == o.rate
            && hours == o.hours && deals == o.deals
            && weighted_hours == o.weighted_hours;
    }
};

// Signed daily specialty rates, written into the aggregator's own chain
// (records.md §11.2): the block author IS the aggregator. Only settled deals
// count; every figure is re-checkable against the chains.
struct DailyAggregate {
    static constexpr RecordType TYPE = RecordType::DailyAggregate;

    int64_t                date;       // UTC day start (ts − ts mod 86400)
    std::vector<RateEntry> rates;      // RAW rates (normalized = rate / W)
    int64_t                timestamp;  // Unix timestamp UTC
    // v3 (economy.md §2б): hours-weighted mean of the day's raw rates. The client
    // divides each rate by W so the network's average labour-hour equals 1 — the
    // money supply grows in step with hours actually worked. Old (4-field) blocks
    // decode with W = 1.0 (no normalization).
    double                 W = 1.0;
    // v4 (ИР-021): the counterparty-independence parameters this aggregate was
    // computed with ("v1;window_days=365;max_cycle=4;..."), so a witness
    // recomputes by the same methodology instead of guessing it — the same
    // discipline as SpecialtyCloud::params. Empty: no weighting was applied.
    std::string            indep;
};

// One neighbour of an activity in the specialty cloud (ИР-018, specialty-axes.md
// §10): a nearby (specialty, activity) slug and the proximity weight.
struct CloudNeighbor {
    std::string slug;   // neighbour activity slug (global rate key)
    double      weight; // proximity 0..1 (closer → larger), used for rate priors

    bool operator==(const CloudNeighbor& o) const noexcept {
        return slug == o.slug && weight == o.weight;
    }
};

// One point of the specialty cloud: an activity, its tree parent (for drift) and
// its k nearest neighbours. Coordinates are NOT published (they live in the
// catalog; the full map is a preview) — only what the rate prior needs.
struct CloudPoint {
    std::string                slug;      // activity = rate key (records.md §11.2, records.md §14.8)
    std::string                parent;    // tree parent slug ("" = none) — drift target
    std::vector<CloudNeighbor> neighbors; // k nearest, by cloud distance
};

// Signed specialty cloud, written into the aggregator's own chain — twin of
// DailyAggregate (specialty-axes.md §10, ИР-018). Neighbours + parents drive rate
// priors for thin/new activities; `snapshot` commits the input block set so anyone
// recomputes and verifies. Every figure is re-checkable against the chains.
struct SpecialtyCloud {
    static constexpr RecordType TYPE = RecordType::SpecialtyCloud;

    int64_t                             date;      // UTC day start
    std::array<uint8_t, 32>             snapshot;  // Merkle root of the input block set
    std::string                         params;    // algorithm version + parameters
    std::vector<std::array<uint8_t, 32>> sources;  // catalog source chains used
    std::vector<CloudPoint>             points;
    int64_t                             timestamp; // Unix timestamp UTC
};

// Attestation of a declared cloud axis value (ИР-019): a practitioner states, on a
// signed record, that an activity's axis (e.g. danger) is `value`. The aggregator
// takes the grade-weighted median over practitioners — the value is set by the
// people who do the work, not by decree, and re-checkable against the chains.
struct AxisAttestation {
    static constexpr RecordType TYPE = RecordType::AxisAttestation;

    std::string activity;   // activity slug (cloud/rate key)
    std::string axis;       // axis name ("danger", ...)
    // Standalone (no `deal`): the ABSOLUTE attested value, 0..1.
    // Deal-backed (`deal` set): a DELTA to the catalog's bootstrap value for this
    // axis (+0.2 = "this job was more dangerous than the standard profile"). A
    // delta says what it means directly, sums into a median, and is anchored to a
    // value fixed in advance — so it cannot chase its own output.
    double      value;
    Ref         grade;      // attester's Grade IN this activity (weight + standing)
    int64_t     timestamp;  // Unix timestamp UTC
    // v2 (ИР-020): the settled Acceptance this profile was written against.
    // Its presence is what makes the statement expensive — two signatures and a
    // real payment stand behind it, not a free opinion. WHICH SIDE the author
    // spoke for is DERIVED, never declared: the Acceptance's author is the payer,
    // `Acceptance::work` names the worker's chain, and an author who is neither is
    // not a party to the deal and does not get a voice in it. A declared side
    // could lie; a derived one cannot.
    std::optional<Ref> deal;
    // v3 (ИР-020): a free-text line FOR PEOPLE — "варил в резервуаре, вытяжки
    // нет". Never parsed, never weighed, never fed to a fit: it can only be read
    // by whoever wants to understand why the number is what it is. The moment a
    // machine reads it, people start writing for the machine. Empty = nothing to
    // add; the field is then absent from the encoding, so old records keep their
    // exact bytes and hashes.
    std::string note;
};

// ── Axis prices (ИР-020) ─────────────────────────────────────────────────────
//
// The hedonic decomposition of observed rates, published the way DailyAggregate
// publishes rates: signed, dated, and committed to its input via `snapshot`, so
// any witness recomputes it instead of trusting it.
//
// LOAD-BEARING INVARIANT: β is an OBSERVATION, never a law. Nothing in the
// protocol may price a deal by this vector. The moment "price = f(profile)"
// holds, profiles get drawn instead of measured (Goodhart) and the residual —
// the entire diagnostic value — vanishes by construction.

// One fitted price vector. `kind` says whose declarations it was fitted on, so
// two sides of a deal with opposite interests can be fitted separately and
// compared instead of averaged into a single number that hides the dispute:
//   "declared" — the catalog profile ⊕ attested overrides (ИР-019), one per
//                activity: today's only source;
//   "seller" / "buyer" / "agreed" — the two-sided profile of ИР-020, later.
struct AxisFitEntry {
    std::string         kind;
    std::vector<double> beta;    // aligned with AxisPrices::basis, column for column
    double              r2     = 0.0;
    uint64_t            rows    = 0;   // observations the fit stood on
    double              weight  = 0.0; // Σ of their weights (hours)

    bool operator==(const AxisFitEntry& o) const noexcept {
        return kind == o.kind && beta == o.beta && r2 == o.r2
            && rows == o.rows && weight == o.weight;
    }
};

// The admission exam for one candidate axis — the audit trail of the decision
// "this axis belongs in the basis". Published because two witnesses that differ
// on a basis must be able to see WHERE they differ, not just that they do.
struct AxisGateEntry {
    std::string axis;
    double      loo_base = 0.0;  // sliding-control error without the candidate
    double      loo_with = 0.0;  // with it
    double      bar      = 0.0;  // loo_base · (1 − margin): what it had to beat
    bool        admitted = false;

    bool operator==(const AxisGateEntry& o) const noexcept {
        return axis == o.axis && loo_base == o.loo_base && loo_with == o.loo_with
            && bar == o.bar && admitted == o.admitted;
    }
};

// Signed axis prices for one day — twin of DailyAggregate and SpecialtyCloud.
struct AxisPrices {
    static constexpr RecordType TYPE = RecordType::AxisPrices;

    int64_t                    date;      // UTC day start
    std::array<uint8_t, 32>    snapshot;  // commits the input (catalog + block set)
    std::string                params;    // algorithm version + every parameter
    // Design columns in canonical order; basis[0] is the constant ("base"), the
    // price of a plain hour. These are PROTOCOL KEYS joining to the catalog's
    // `axes` and to AxisAttestation::axis — not display names.
    std::vector<std::string>   basis;
    std::vector<AxisFitEntry>  fits;
    std::vector<AxisGateEntry> gate;
    int64_t                    timestamp; // Unix timestamp UTC
    // v2 (ИР-020): how far the two sides of a deal stand apart on each column,
    // aligned with `basis` (0 for the constant, and for columns nobody disputed).
    // Published rather than averaged away: a value both a buyer and a seller state
    // is evidence, a value only one of them states is a position, and the gap
    // between them is the first signal that an axis is really two axes glued
    // together. Empty until the two-sided profile exists.
    std::vector<double>        disagreement;
    // v3 (ИР-020): how firmly the network agrees on each axis price — the spread
    // of βj across leave-one-out folds, column for column with `basis`.
    //
    // This REPLACES the per-basket residual as the published diagnostic. The
    // residual asked "how far is this basket from the model", a question that
    // stops meaning anything once specialty and grade dissolve into axes: then no
    // two pieces of work are "the same work" and a basket has nothing to be
    // compared against. The AXIS is still shared even when the work is not, so
    // the question that survives is about it. Small spread: the whole economy
    // agrees on that price. Large: it rests on a handful of deals and is not yet
    // a fact about the network.
    std::vector<double>        spread;
};

// Profile of ONE piece of work, declared in the deal itself (ИР-022).
//
// The catalog gives a profile per ACTIVITY, which is why a grade had to exist:
// it was the only thing that varied inside a specialty, so an electrician's hour
// at 220 V and at 400 V were the same row. Here the work describes itself, and
// the grade stops having anything to say — an axis "work under 400 V" says it
// directly, and better.
//
// A SET with its own hash, not a scatter of one-axis records: that is what makes
// it reusable. `base` points at an earlier profile to inherit wholesale, so a
// settled description of a job becomes what a "profession" used to be — a
// reference to a description that worked, not a category from a directory.
//
// THE BREAKDOWN IS THE POINT (ИР-022, 2026-09-09). Each axis carries the labour
// hours of the price that went to it, and they add up to the price exactly. So
// there is no residual — not because a model fits well, but because there is
// nothing to add up beyond what was named. Nothing is inferred, so there is
// nothing to draw a profile against (Goodhart), and the price of an axis is not
// decreed anywhere: what the network can say is only how much of all the labour
// it paid actually went to danger, which is a fact, not an estimate.
//
// There is deliberately NO "other" bucket. Something you cannot name, you name:
// invent an axis and argue for it. A badly argued axis reused again and again is
// itself the signal — and a better one than a nameless remainder, because a
// remainder ends the conversation while a bad axis invites it.
//
// Deliberately a separate type rather than more AxisAttestation records: an
// attestation states one axis of an activity in general, a profile states the
// whole shape of one job, and only the second can be pointed at by hash.
struct DealProfileAxis {
    std::string axis;    // axis slug (docs/catalogs/axes.json, later a chain Ref)
    // MANDATORY: labour hours of this deal's price that went to this axis. The
    // breakdown must add up to the deal's `labor_units` exactly — this is the
    // PAYMENT, itemized, not an estimate of one. Nothing is inferred from it and
    // nothing may contradict it.
    double      units = 0.0;
    // OPTIONAL: how much of the axis was in an hour of this work (intensity).
    // Carries no money. It exists only for GEOMETRY — finding neighbours in the
    // cloud, and suggesting a rate for work nobody has done yet — and it is a
    // CANDIDATE FOR REMOVAL: if it never shows anything the breakdown does not
    // already show, it goes (ИР-022). Absent is a legitimate state, not a gap.
    std::optional<double> value;

    bool operator==(const DealProfileAxis& o) const noexcept {
        return axis == o.axis && units == o.units && value == o.value;
    }
};

struct DealProfile {
    static constexpr RecordType TYPE = RecordType::DealProfile;

    Ref                          deal;   // the settled Acceptance this describes
    std::vector<DealProfileAxis> axes;   // canonical order: sorted by axis slug
    // Inherit an earlier profile and override only what differs — "same as that
    // job, but more dangerous". Absent: the axes here are the whole profile.
    std::optional<Ref>           base;
    std::string                  note;   // a line for people; never parsed
    int64_t                      timestamp = 0;
};

// An axis, defined in somebody's chain (ИР-022).
//
// The vocabulary of labour is not the aggregator's to decide. It used to be a
// file the aggregator shipped, so whoever edited that file decided which facets
// of work exist — the last centralized point left in the valuation of labour,
// after the VALUES became practitioners' (ИР-019) and the PRICES became the
// deals' (ИР-020, ИР-022). Now an axis is an ordinary record: anyone writes one,
// anyone references it, and it gains force by being used, not by being blessed.
//
// THE ARGUMENT IS THE POINT. Nothing stops anyone inventing an axis, so what
// separates a good one from a bad one is how well it is argued: what it means,
// why it is one axis and not two, what it is not. An axis reused again and again
// with nothing written behind it is visible as exactly that (`described` in the
// axis ledger) — and that visibility is the whole enforcement. There is no
// "other" bucket anywhere in the protocol, deliberately: what you cannot name,
// you name.
//
// `same_as` is a MERGE CLAIM, signed like anything else: "this axis and that one
// are the same thing". Anyone may publish one — the author of the original, or
// anybody at all if that author is gone or has lost their keys. Claims are not
// applied automatically; they are weighed the way everything here is weighed, by
// who uses them.
struct AxisDef {
    static constexpr RecordType TYPE = RecordType::AxisDef;

    std::string        slug;         // join key everything references
    std::string        ru;           // display name
    std::string        description;  // the argument: what it means and why it is one
    std::optional<Ref> parent;       // broader axis / family, when there is one
    std::optional<Ref> same_as;      // merge claim, never applied automatically
    int64_t            timestamp = 0;
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
    Material,
    Tool,
    Transfer,
    DailyAggregate,
    Pledge,
    PledgeRevoke,
    Redemption,
    SpecialtyCloud,
    AxisAttestation,
    AxisPrices,
    DealProfile,
    AxisDef
>;

} // namespace records
