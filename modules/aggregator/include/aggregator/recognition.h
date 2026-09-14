#pragma once

// The deals on the table (records.md §9.5, ИР-023). A deal is a Transfer that
// carries the acceptance it pays for: appraisal and payment in one record, so it
// is paid exactly by construction and the acceptor is the payer. Every view
// that reads deals reads them here, so that no two views disagree about which
// deal happened: one work — one deal; a work with two was sold twice and none
// of them counts. The rule itself is records' — the one the client applies
// (records/acceptance.h); this only gathers the claims.

#include "aggregator.h"

#include <records/acceptance.h>
#include <records/types.h>

#include <array>
#include <vector>

namespace aggregator {

struct DealEntry {
    std::array<uint8_t, 32> hash{};    // the deal block: the Transfer carrying the acceptance
    UserId                  payer{};   // the chain that wrote it — who accepted and paid
    records::Transfer       transfer;  // transfer.accept is always set

    const records::Acceptance& accept() const noexcept { return *transfer.accept; }
};

struct DealsOnTable {
    std::vector<DealEntry>   all;          // every deal in the storage, in block-hash order
    records::DealRecognition recognition;  // which of them count

    bool counts(const std::array<uint8_t, 32>& deal) const {
        return recognition.recognized.count(deal) > 0;
    }
};

// One pass over the storage. A deal counts as written only by its own sender
// (the spoof guard every view applies); the codec has already refused one that
// does not pay exactly, pays itself or pays anyone but the worker.
DealsOnTable read_deals(const AggregatorStorage& storage);

} // namespace aggregator
