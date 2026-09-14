#pragma once

// One work — one deal (records.md §9.5, ИР-023).
//
// Since 2026-09-14 an acceptance is not a record of its own but the `accept`
// field of the buyer's Transfer: the appraisal and its payment are one signed
// record, so every deal is paid exactly by construction and the one who accepts
// is the one who pays. What is left to police is the work itself. Two deals
// naming one WorkRecord are one work sold twice: its hours would enter the axis
// prices twice, the carried cost of the means of production would be paid twice
// off a single carry link, and qualification would count one job as «приняли
// две цепи». So:
//
//   a work with one deal      → the deal counts;
//   a work with two or more   → it was sold twice: none counts, and the work
//                               is reported as oversold.
//
// The rule is the same for every reader — aggregator and client — so that they
// never disagree about which deal happened. Selling one work to many buyers in
// portions is the product thread of ИР-023, not several deals on one record.

#include <array>
#include <cstdint>
#include <set>
#include <vector>

namespace records {

// One deal as the rule sees it: a Transfer carrying an acceptance, written by
// its own sender.
struct DealClaim {
    std::array<uint8_t, 32> deal{};   // hash of the Transfer block
    std::array<uint8_t, 32> work{};   // hash of the WorkRecord its acceptance names
};

struct DealRecognition {
    std::set<std::array<uint8_t, 32>> recognized;  // deals that count
    std::set<std::array<uint8_t, 32>> oversold;    // works with more than one deal
};

DealRecognition recognize_deals(const std::vector<DealClaim>& claims);

// Whether a payment is exact: its portions add up to labor_units + carried_units
// (records.md §12.8 "="). A deal pays something — nothing changing hands is not a
// deal, even at a zero price.
bool settled_exactly(double paid, double payable) noexcept;

} // namespace records
