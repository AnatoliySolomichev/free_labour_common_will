#pragma once

// Who gets the result, and which deal counts (records.md §9.5, ИР-023).
//
// A deal is an atomic pair: the seller names the buyer, the buyer accepts and
// pays. The acceptance rides inside the buyer's Transfer — appraisal and payment
// in one record, exact by construction, the one who accepts is the one who pays.
// The seller names the buyer either on the work itself — `WorkRecord.customer`,
// an order agreed in advance (records.md §9.4) — or with a `Release` to that
// buyer, live until its term (records.md §9.8). So, per result:
//
//   a deal the seller did not address → void: no acceptance and no payment,
//                                       whoever wrote it and whatever it paid;
//   addressed deals of one buyer      → the first counts; the others are
//                                       repeats — hours went, nothing counts twice;
//   addressed deals of two buyers     → the seller gave one result twice: none
//                                       counts, and both addresses are the seller's.
//
// Only the one who sold a result can spoil its deal. A stranger willing to pay
// cannot — that was the flaw of the earlier «two deals — none counts» rule.
// One result — one counted deal, because its hours would otherwise enter the axis
// prices twice, the carried cost be paid twice off one carry link, and one job
// count as «приняли две цепи».
//
// The rule is the same for every reader — aggregator and client — so that they
// never disagree about which deal happened. Selling one lot to many buyers in
// portions is the lot sub-thread of ИР-023, not several deals on one record.

#include "records/types.h"

#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

namespace records {

// Whether the seller named the payer of `deal` as its buyer:
//   customer — the accepted WorkRecord's `customer`, as its worker wrote it
//              (nullopt: none, or the work is not on the table);
//   release  — the Release that the deal's accept.release names, as its seller
//              wrote it (nullptr: the deal names none, or it is not on the table).
// A deal naming a release is judged by that release alone: it must release the
// accepted work, to the payer, and the deal must come before its term. A deal
// naming none is addressed when the payer is the work's customer. Whoever cannot
// find the seller's records gets «not addressed» — the deal does not count yet.
bool addressed_to(const Transfer& deal,
                  const std::optional<std::array<uint8_t, 32>>& customer,
                  const Release* release) noexcept;

// One deal as the rule sees it: a Transfer carrying an acceptance, written by
// its own sender.
struct DealClaim {
    std::array<uint8_t, 32> deal{};           // hash of the Transfer block
    std::array<uint8_t, 32> work{};           // hash of the WorkRecord — the result sold
    std::array<uint8_t, 32> payer{};          // who accepted and paid
    int64_t                 timestamp = 0;    // the deal's own
    bool                    addressed = false;  // addressed_to(...)
};

struct DealRecognition {
    std::set<std::array<uint8_t, 32>> recognized;   // deals that count
    std::set<std::array<uint8_t, 32>> repeated;     // the same buyer paid again
    std::set<std::array<uint8_t, 32>> contested;    // deals on a result given to two buyers
    std::set<std::array<uint8_t, 32>> unaddressed;  // void: the seller named no such buyer
    std::set<std::array<uint8_t, 32>> oversold;     // WORKS given to two buyers or more

    // Hours really moved: every addressed deal — the one that counts, a repeat,
    // or one of a double sale. A void deal moves nothing.
    bool moves_hours(const std::array<uint8_t, 32>& deal) const {
        return recognized.count(deal) > 0 || repeated.count(deal) > 0
            || contested.count(deal) > 0;
    }
};

DealRecognition recognize_deals(const std::vector<DealClaim>& claims);

// Whether a payment is exact: its portions add up to labor_units + carried_units
// (records.md §12.8 "="). A deal pays something — nothing changing hands is not a
// deal, even at a zero price.
bool settled_exactly(double paid, double payable) noexcept;

} // namespace records
