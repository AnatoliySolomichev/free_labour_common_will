#pragma once
// Test support (records.md §9.4 v4, ИР-023): a deal counts only on a work that
// sits in its worker's goods thread, on hours no other work of the chain claims.
// Fixtures write many works per worker without caring about time, so each
// fixture's add() passes records through here: a WorkRecord written with neither
// a start nor a thread link is placed right after the worker's previous work and
// linked to it. A work written with its own start_ts or link is left exactly as
// written — that is how a test breaks the thread on purpose.

#include <records/goods_thread.h>
#include <records/types.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <variant>

class LaborThreads {
public:
    // Before the record is encoded: its place in `worker`'s thread.
    records::Record stamp(const std::array<uint8_t, 32>& worker,
                          const records::Record& rec) const {
        records::Record out = rec;
        auto* wr = std::get_if<records::WorkRecord>(&out);
        if (!wr || wr->thread || wr->start_ts != 0) return out;
        records::ThreadLink link{};
        const auto it = tips_.find(worker);
        if (it == tips_.end()) {
            wr->start_ts = kEpoch;
        } else {
            wr->start_ts = it->second.free_from;
            link.seq     = it->second.seq + 1;
            link.prev    = {it->second.link};
        }
        wr->thread = link;
        return out;
    }

    // After the block is added: it now continues `worker`'s thread.
    void placed(const std::array<uint8_t, 32>& worker, const records::Record& rec,
                const std::array<uint8_t, 32>& block_hash) {
        const auto* wr = std::get_if<records::WorkRecord>(&rec);
        if (!wr || !wr->thread) return;
        const int64_t end = records::labor_interval(*wr).second;
        const auto    it  = tips_.find(worker);
        if (it == tips_.end()) {
            tips_[worker] = Tip{records::Ref{worker, block_hash}, wr->thread->seq,
                                std::max(end, kEpoch)};
            return;
        }
        if (wr->thread->seq >= it->second.seq) {
            it->second.link = records::Ref{worker, block_hash};
            it->second.seq  = wr->thread->seq;
        }
        it->second.free_from = std::max(it->second.free_from, end);
    }

private:
    static constexpr int64_t kEpoch = 1'000'000'000;   // an arbitrary past: 2001-09-09
    struct Tip {
        records::Ref link;
        uint64_t     seq       = 0;
        int64_t      free_from = kEpoch;
    };
    std::map<std::array<uint8_t, 32>, Tip> tips_;
};
