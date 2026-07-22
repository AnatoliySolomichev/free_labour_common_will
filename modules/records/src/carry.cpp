#include "records/carry.h"

#include <algorithm>
#include <cmath>

namespace records {

namespace {
constexpr double EPS = 1e-6;
}

double carry_step(double cost, double capacity, double used,
                  double collected_before) noexcept {
    if (cost <= 0 || capacity <= 0 || used <= 0) return 0;
    const double remainder = cost - collected_before;
    if (remainder <= 0) return 0;   // fully recovered — forcibly free (§9.4)
    return std::min(used / capacity * cost, remainder);
}

CarryHistory carry_history(std::vector<ObservedCarry> links,
                           double cost, double capacity) {
    CarryHistory h{};
    if (links.empty()) return h;

    std::sort(links.begin(), links.end(),
              [](const ObservedCarry& a, const ObservedCarry& b) {
                  if (a.entry.seq != b.entry.seq) return a.entry.seq < b.entry.seq;
                  return a.block_hash < b.block_hash;
              });
    // The same block can arrive via merge and via fetch — not equivocation.
    links.erase(std::unique(links.begin(), links.end(),
                            [](const ObservedCarry& a, const ObservedCarry& b) {
                                return a.entry.seq == b.entry.seq &&
                                       a.block_hash == b.block_hash;
                            }),
                links.end());

    bool     first     = true;
    uint64_t prev_seq  = 0;
    double   prev_after = 0;
    for (const auto& o : links) {
        const CarryEntry& e = o.entry;
        if (!first && e.seq == prev_seq) {
            // One seq in two different blocks: the thread forked — the asset
            // was charged twice over parallel branches.
            if (h.equivocated_seqs.empty() ||
                h.equivocated_seqs.back() != e.seq)
                h.equivocated_seqs.push_back(e.seq);
            continue;  // digest the first-seen block of the forked link
        }

        ++h.links_seen;

        // Per-link recognition: the link itself declares collected_before as
        // after − carried, so the formula is checkable in isolation.
        const double before   = e.after - e.carried;
        const double expected = carry_step(cost, capacity, e.used, before);
        if (std::abs(e.carried - expected) > EPS) h.formula_mismatch = true;
        if (e.after > cost + EPS)                 h.over_invariant   = true;

        if (!first) {
            if (e.after + EPS < prev_after) h.after_decreasing = true;
            // Continuity is objective only between adjacent seqs actually
            // seen; across a gap the missing links explain any jump.
            if (e.seq == prev_seq + 1 &&
                std::abs(prev_after + e.carried - e.after) > EPS)
                h.thread_inconsistent = true;
        }

        first      = false;
        prev_seq   = e.seq;
        prev_after = e.after;
        h.collected = e.after;
    }

    h.links_expected = prev_seq + 1;
    h.gaps           = h.links_seen < h.links_expected;
    return h;
}

} // namespace records
