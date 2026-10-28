#pragma once
// Finality of a deal with a window (blockchain.md §11.10, решения участника 2026-10-28).
// The buyer names the window in the offer, the seller accepts it by answering;
// it runs from the seller's answer by the seller's clock. A revocation of the
// buyer's paying key that came here within the window and left the offer off
// the owner's line (blockchain.md §6.7 rule 12) voids the deal; a fork of the paying
// branch that came within the window freezes it until an ancestor names the
// line (rule 13). After the window the deal is final, whatever comes later:
// the loss is the key owner's, bounded by the branch's purse (economy.md §5а).
#include "aggregator.h"
#include <optional>

namespace aggregator {

enum class Finality : uint8_t {
    pending,  // within the window, nothing against it yet
    final,    // the window is over and nothing came within it
    voided,   // a revocation within the window left the offer off the owner's line
    frozen,   // a fork within the window, the owner's line not named yet
};

const char* finality_name(Finality f) noexcept;

struct DealFinality {
    Finality                 state    = Finality::pending;
    Timestamp                final_at = 0;  // the seller's answer + the window
    std::optional<Timestamp> revoked_at;    // when the voiding revocation came here
};

// `offer` — the buyer's deal block; `answered` — the time of the seller's
// accepting answer as the seller wrote it (the seller's clock). The
// revocations are the certificates in this aggregator's warehouse, checked
// here; when each came — the arrival it was noted with.
DealFinality deal_finality(const AggregatorStorage& storage, const Block& offer,
                           Timestamp answered, int64_t window, Timestamp now);

} // namespace aggregator
