#pragma once

// One work — one recognized acceptance (records.md §9.5, ИР-023).
//
// An acceptance certifies that the work was received and appraises it. Two
// acceptances of one work are one work sold twice: its hours would enter the
// axis prices twice, the carried cost of the means of production would be paid
// twice off a single carry link, and qualification would count one job as
// "accepted by two chains". So at most one acceptance per work counts, and the
// rule is the same for every reader — aggregator and client — so that they never
// disagree about which deal happened:
//
//   exactly one settled acceptance  → it counts;
//   two or more settled             → the work was sold twice: none counts,
//                                     and the work is reported as oversold;
//   none settled                    → the work's acceptance counts if it is
//                                     the only one.
//
// Settled beats unsettled, so an acceptance nobody paid for cannot push out an
// honest one: it costs nothing to write and weighs nothing. Making a work
// "sold twice" takes paying for it for real.

#include <array>
#include <cstdint>
#include <set>
#include <vector>

namespace records {

// One acceptance as the rule sees it. Self-acceptances and acceptances written
// by someone other than their receiver are not claims at all: the first vouch
// for nothing (records.md §14.4), the second are forgeries.
struct AcceptanceClaim {
    std::array<uint8_t, 32> acceptance{};     // hash of the Acceptance block
    std::array<uint8_t, 32> work{};           // hash of the WorkRecord it appraises
    bool                    settled = false;  // paid exactly (settled_exactly)
};

struct AcceptanceRecognition {
    std::set<std::array<uint8_t, 32>> recognized;  // acceptances that count
    std::set<std::array<uint8_t, 32>> oversold;    // works settled more than once
};

AcceptanceRecognition recognize_acceptances(const std::vector<AcceptanceClaim>& claims);

// Whether an acceptance is paid exactly: the transfers naming it in `reason`
// add up to labor_units + carried_units (records.md §12.8 "="). With no payment
// at all it is not settled, even at a zero price — nothing changed hands.
bool settled_exactly(double paid, double payable) noexcept;

} // namespace records
