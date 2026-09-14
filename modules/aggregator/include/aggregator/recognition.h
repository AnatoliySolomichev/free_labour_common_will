#pragma once

// The deals on the table (records.md §9.5, ИР-023). A deal is a Transfer that
// carries the acceptance it pays for: appraisal and payment in one record, so it
// is paid exactly by construction and the acceptor is the payer. It happens only
// when the seller addressed the result to that payer — the work's customer
// (records.md §9.4) or a live release to them (records.md §9.8); a deal nobody
// addressed is void. It counts only on a work in its worker's goods thread, on
// hours no other work of that chain claims (records.md §9.4 v4). Every view
// that reads deals reads them here, so that no two views disagree about which
// deal happened. The rules themselves are records' — the ones the client
// applies (records/acceptance.h, records/goods_thread.h); this only gathers the
// seller's records and the buyers' claims.

#include "aggregator.h"

#include <records/acceptance.h>
#include <records/goods_thread.h>
#include <records/types.h>

#include <array>
#include <map>
#include <optional>
#include <vector>

namespace aggregator {

struct DealEntry {
    std::array<uint8_t, 32> hash{};    // the deal block: the Transfer carrying the acceptance
    UserId                  payer{};   // the chain that wrote it — who accepted and paid
    records::Transfer       transfer;  // transfer.accept is always set

    const records::Acceptance& accept() const noexcept { return *transfer.accept; }
};

// The seller's side of a deal.
struct WorkOnTable {
    UserId              worker{};   // the chain that signed the WorkRecord
    records::WorkRecord record;     // its customer (v3), thread link and proposed axes (v4)
};

struct ReleaseEntry {
    std::array<uint8_t, 32> hash{};    // the Release block
    UserId                  seller{};  // who wrote it — always the worker of its work
    records::Release        release;
};

struct DealsOnTable {
    std::vector<DealEntry>   all;          // every deal in the storage, in block-hash order
    records::DealRecognition recognition;  // which of them count, which are void
    std::map<std::array<uint8_t, 32>, WorkOnTable> works;   // every WorkRecord by block hash
    std::vector<ReleaseEntry> releases;    // releases written by the worker of their work
    std::map<UserId, records::LaborThread> threads;   // each chain's goods thread, labor links

    // The deal that counts for its result: statistics read only these.
    bool counts(const std::array<uint8_t, 32>& deal) const {
        return recognition.recognized.count(deal) > 0;
    }
    // Hours really moved: every addressed deal. Money views read these; a void
    // deal neither pays nor settles anything.
    bool moves_hours(const std::array<uint8_t, 32>& deal) const {
        return recognition.moves_hours(deal);
    }
    // A work whose deal may count: on the table in the chain its ref names, in
    // that chain's thread, on hours of its own (records.md §9.4 v4).
    bool sound(const records::Ref& work) const {
        const auto w = works.find(work.hash);
        if (w == works.end() || w->second.worker.bytes != work.chain) return false;
        const auto t = threads.find(w->second.worker);
        return t != threads.end() && t->second.sound(work.hash);
    }
};

// One pass over the storage. A deal counts as written only by its own sender and
// a release only by the worker of its work (the spoof guards every view
// applies); the codec has already refused a deal that does not pay exactly,
// pays itself or pays anyone but the worker.
DealsOnTable read_deals(const AggregatorStorage& storage);

} // namespace aggregator
