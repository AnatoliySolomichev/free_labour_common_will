#include "records/goods_thread.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace records {

std::pair<int64_t, int64_t> labor_interval(const WorkRecord& wr) noexcept {
    const double seconds = wr.hours > 0.0 && std::isfinite(wr.hours) ? wr.hours * 3600.0 : 0.0;
    return {wr.start_ts, wr.start_ts + static_cast<int64_t>(std::llround(seconds))};
}

bool intervals_cross(const std::pair<int64_t, int64_t>& a,
                     const std::pair<int64_t, int64_t>& b) noexcept {
    return a.first < b.second && b.first < a.second;
}

LaborThread check_labor_thread(const std::array<uint8_t, 32>& chain,
                               const std::vector<ObservedWork>& works) {
    using Hash32 = std::array<uint8_t, 32>;
    std::map<Hash32, const ObservedWork*> by_hash;
    for (const auto& w : works) by_hash.emplace(w.hash, &w);

    LaborThread out;

    // Links: each checks out against the prev it names, when those are on the
    // table. A prev we have not received is a gap, not a fault — the link is
    // given the benefit of the doubt, as the carry history does (records.md §9.4).
    for (const auto& [hash, w] : by_hash) {
        if (!w->link) { out.unthreaded.insert(hash); continue; }
        bool     ok        = true;
        bool     all_known = true;
        uint64_t top       = 0;
        for (const auto& p : w->link->prev) {
            if (p.chain != chain) { ok = false; break; }         // a thread is one chain's
            const auto it = by_hash.find(p.hash);
            if (it == by_hash.end()) { all_known = false; out.gaps = true; continue; }
            if (!it->second->link) { ok = false; break; }        // a prev must be a link
            top = std::max(top, it->second->link->seq);
        }
        if (ok && all_known && !w->link->prev.empty() && w->link->seq != top + 1) ok = false;
        (ok ? out.linked : out.broken).insert(hash);
    }

    // Forks and tips, over the links that check out.
    std::map<Hash32, std::size_t> continued;
    for (const auto& hash : out.linked)
        for (const auto& p : by_hash.at(hash)->link->prev)
            if (out.linked.count(p.hash)) ++continued[p.hash];
    for (const auto& [hash, n] : continued)
        if (n > 1) ++out.forks;
    for (const auto& hash : out.linked)
        if (!continued.count(hash)) out.tips.push_back(hash);

    // Hours: no two works of the chain claim the same time — threaded or not,
    // two signed intervals that cross are the proof by themselves. Sweep by
    // start, keeping the work that reaches furthest: any work crossing an
    // earlier one crosses that one too.
    std::vector<const ObservedWork*> by_start;
    for (const auto& [hash, w] : by_hash)
        if (w->interval.second > w->interval.first) by_start.push_back(w);   // hours claimed
    std::sort(by_start.begin(), by_start.end(), [](const ObservedWork* a, const ObservedWork* b) {
        return a->interval.first != b->interval.first ? a->interval.first < b->interval.first
                                                      : a->hash < b->hash;
    });
    const ObservedWork* reach = nullptr;
    for (const auto* w : by_start) {
        if (reach && w->interval.first < reach->interval.second) {
            out.overlapping.insert(reach->hash);
            out.overlapping.insert(w->hash);
            out.overlaps.emplace_back(reach->hash, w->hash);
        }
        if (!reach || w->interval.second > reach->interval.second) reach = w;
    }
    return out;
}

} // namespace records
