#pragma once

// Axis ledger (ИР-022) — where the labour of the network actually went.
//
// This is the mechanism that replaces the fitted price of an axis. A deal states
// how many labour hours of its price went to each axis, and the breakdown adds up
// to the price EXACTLY. So there is no residual — not because a model fits well,
// but because there is nothing to add up beyond what the parties named. Nothing
// is inferred, so there is nothing to draw a profile against (Goodhart), and no
// price is decreed anywhere.
//
// What the network can then say is not an estimate but a fact:
//
//   share  — of all the labour hours paid, this fraction went to danger;
//   per_hour — averaged over the work that named it, an hour of such work paid
//              this many standard hours for danger;
//   spread — how differently deals paid for it, straight from what they said,
//            with no leave-one-out and no fitting.
//
// A wide spread does NOT mean the model is poor. It means the network does not
// actually agree that this is one axis — the signal to split it in two.
//
// There is deliberately NO "other" bucket. What you cannot name, you name:
// invent an axis and argue for it. An axis reused again and again with no
// argument behind it is itself the signal, and a better one than a nameless
// remainder — a remainder ends the conversation, a bad axis invites it.

#include "aggregator.h"

#include <records/catalog.h>
#include <records/types.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace aggregator {

// One axis, as the settled deals actually paid it.
struct AxisLedgerRow {
    std::string slug;
    double      units    = 0.0;  // Σ labour hours that went to this axis
    double      hours    = 0.0;  // Σ hours of the work that named it
    uint64_t    deals    = 0;    // how many settled deals named it
    uint64_t    chains   = 0;    // how many DISTINCT chains named it — one busy
                                 // pair is one voice's worth of evidence, not many
    double      share    = 0.0;  // units / Σ units over all axes
    double      per_hour = 0.0;  // units / hours
    double      spread   = 0.0;  // hours-weighted std of per-deal units/hours
    // False when no catalog entry describes this axis. Not an error: anyone may
    // invent an axis. It is the visible fact that it was used without argument —
    // and an unargued axis reused again and again is exactly what to look at.
    bool        described = false;
};

// Read the ledger off the settled deals.
//
// A profile counts only when the deal is SETTLED, the author is a party to it
// (derived from the Acceptance, never declared), and the breakdown ADDS UP to the
// deal's labor_units. The last check is what makes the itemization a statement
// about the payment rather than an opinion beside it: a breakdown that does not
// sum to the price describes some other deal.
//
// `catalogs` is consulted only to mark `described`; it never gates a row.
std::vector<AxisLedgerRow> build_axis_ledger(
    const AggregatorStorage&                    storage,
    const std::vector<records::Catalog>*        catalogs = nullptr);

} // namespace aggregator
