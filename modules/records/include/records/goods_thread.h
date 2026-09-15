#pragma once
// The goods thread of a chain, labor links (records.md §9.4 v4, ИР-023): each
// work is a link {seq, prev} of the chain's one thread, and the invariant is
// that no two works of the chain claim the same hours. Pure computation, like
// carry.h: the caller hands over the works it holds; a partial view is
// legitimate and reported as such (`gaps`), never presented as the full picture.
//
// The fork rule is lenient for labor (ИР-023, decided 2026-09-12): a link says
// only «I worked from 10 to 18» and asserts nothing about other links, so two
// links continuing one prev — a phone and a laptop that did not see each other —
// are no fraud while their hours do not cross. Crossing hours are: two signed
// intervals of one chain that share time are the proof by themselves, whichever
// of them is fictional.

#include "records/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace records {

// The interval a work claims, in seconds: [start_ts, start_ts + hours·3600).
std::pair<int64_t, int64_t> labor_interval(const WorkRecord& wr) noexcept;

// Whether two claimed intervals share any time. Touching ends do not: a work
// may start the second the previous one ended.
bool intervals_cross(const std::pair<int64_t, int64_t>& a,
                     const std::pair<int64_t, int64_t>& b) noexcept;

// One work as seen in a block of the chain.
struct ObservedWork {
    std::array<uint8_t, 32>     hash{};     // the WorkRecord block
    std::pair<int64_t, int64_t> interval;   // labor_interval(...)
    std::optional<ThreadLink>   link;       // absent: written outside the thread
};

struct LaborThread {
    std::set<std::array<uint8_t, 32>> linked;       // links that check out, or cannot be checked yet
    std::set<std::array<uint8_t, 32>> unthreaded;   // works written outside the thread
    std::set<std::array<uint8_t, 32>> broken;       // seq ≠ 1 + max(prev), or a prev that is no link of this chain
    std::set<std::array<uint8_t, 32>> overlapping;  // works whose hours cross another work's
    // One signed proof per crossing found: the earlier-starting work first.
    std::vector<std::pair<std::array<uint8_t, 32>, std::array<uint8_t, 32>>> overlaps;
    std::vector<std::array<uint8_t, 32>> tips;      // links no link continues, ascending
    std::size_t forks = 0;                          // links continued by more than one link
    bool        gaps  = false;                      // some prev is not on the table

    // A work whose deal may count: in the thread, on hours of its own.
    bool sound(const std::array<uint8_t, 32>& work) const {
        return linked.count(work) > 0 && overlapping.count(work) == 0;
    }
};

// Digest the works of ONE chain — any order; the same block seen twice is one.
LaborThread check_labor_thread(const std::array<uint8_t, 32>& chain,
                               const std::vector<ObservedWork>& works);

// ── Lots: an output sold in portions (records.md §9.8 v2, records.md §9.9) ───

// The part of a work that a portion of its lot carries: the output's share of
// the work's hours (1 when the work has one output) × qty / the output's qty.
// nullopt when the portion cannot be valued: no such output, an output of no
// size, or several outputs without declared shares.
std::optional<double> portion_fraction(const WorkRecord& wr, const LotLink& lot) noexcept;

// One link of a lot's sub-thread as seen in a block of the chain: the release of
// a portion, or a write-off.
struct ObservedLotLink {
    std::array<uint8_t, 32> hash{};          // the Release or WriteOff block
    LotLink                 link;
    int64_t                 timestamp = 0;   // the record's own
    std::optional<int64_t>  expires;         // releases only — a write-off is final
    bool                    taken = false;   // a release some addressed deal completed
};

struct LotState {
    double qty = 0;                                 // the lot: the output's size
    std::set<std::array<uint8_t, 32>> valid;        // links that check out, or cannot be checked yet
    std::set<std::array<uint8_t, 32>> invalid;      // over the lot, arithmetic off, a false return, equivocated
    std::vector<uint64_t> equivocated_seqs;         // one seq in two blocks: two sales from one remainder
    bool   gaps = false;                            // links or returns not on the table
    double used = 0;                                // taken from the lot at the newest valid link
    std::set<std::string> held;                     // numbered units taken and not returned

    double remaining() const noexcept { return qty - used; }
};

// Digest the sub-thread of ONE lot of `chain` — any order; the same block seen
// twice is one. Strict like the carry thread: a link checks out only against
// its own previous link, and two links with one seq are both refused. A return
// is honest only for an earlier release of this lot that expired before the
// returning link, was taken by nobody and is returned once. For a lot counted
// piece by piece (`units`, ИР-023, вопрос 10) every link names the units it
// takes; a unit the lot has not, or one somebody holds, is sold twice.
LotState check_lot(const std::array<uint8_t, 32>& chain, double qty,
                   const std::vector<ObservedLotLink>& links,
                   const std::vector<std::string>& units = {});

// What a portion of a lot costs — take it or leave it (records.md §9.8 v3,
// ИР-023, вопрос 11): its share of the lot's price tag, broken down by the
// worker's proposed axes, with its share of the work's hours and of the carried
// cost. nullopt when the lot has no tag, the work no proposed breakdown, or the
// portion cannot be valued — such a lot is not sold in portions.
struct PortionTerms {
    double hours_raw     = 0;
    double labor_units   = 0;
    double carried_units = 0;
    std::vector<AcceptanceAxis> axes;   // by axis hash; Σ units = labor_units exactly
};
std::optional<PortionTerms> portion_terms(const WorkRecord& wr, const LotLink& lot);

// Whether an acceptance pays exactly on those terms. Bargaining happens over an
// order or a lot bought whole; a portion from stock is bought or not.
bool meets_terms(const Acceptance& a, const PortionTerms& t) noexcept;

// ── Resale: a bought lot sold on (records.md §9.8 v4, ИР-023) ───────────────

// One lot a purchase put in the buyer's hands: an output of the product, how
// much of it and which units, and what it cost the buyer.
struct HoldingLot {
    uint32_t                 out  = 0;
    double                   qty  = 0;
    std::vector<std::string> units;
    double                   cost = 0;   // labor-hours paid for it
};

// The lots of a purchase. `purchase` — the buyer's deal; `work` — the product's
// birth record; `bought` — the release the deal completed (nullptr: bought
// whole, by an order or a whole release). A portion bought is one lot; a whole
// result — one lot per output, its cost split by the outputs' shares; a service
// leaves nothing to sell on.
std::vector<HoldingLot> holding_lots(const Transfer& purchase, const WorkRecord& work,
                                     const Release* bought);

// What a portion of a bought lot costs sold on: its share of what the holder
// paid — no markup and no labor. The axes were settled by the first sale; the
// second is take it or leave it (решение участника 2026-09-15).
double resale_price(const HoldingLot& lot, double qty) noexcept;

// Whether an acceptance pays a resale exactly: carried cost only.
bool meets_resale(const Acceptance& a, double price) noexcept;

} // namespace records
