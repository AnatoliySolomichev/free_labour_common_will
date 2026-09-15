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

std::optional<double> portion_fraction(const WorkRecord& wr, const LotLink& lot) noexcept {
    if (lot.out >= wr.outputs.size()) return std::nullopt;
    const ResourceQty& o = wr.outputs[lot.out];
    if (!(o.qty > 0.0)) return std::nullopt;
    if (!o.share && wr.outputs.size() > 1) return std::nullopt;
    return (o.share ? *o.share : 1.0) * lot.qty / o.qty;
}

LotState check_lot(const std::array<uint8_t, 32>& chain, double qty,
                   const std::vector<ObservedLotLink>& links,
                   const std::vector<std::string>& units) {
    using Hash32 = std::array<uint8_t, 32>;
    constexpr double EPS = 1e-6;   // the carry thread's tolerance (records.md §9.4)

    LotState st;
    st.qty = qty;
    std::map<Hash32, const ObservedLotLink*> by_hash;
    for (const auto& l : links) by_hash.emplace(l.hash, &l);

    std::map<uint64_t, std::size_t> per_seq;
    std::map<Hash32, std::size_t>   returned;   // release → links returning it
    for (const auto& [hash, l] : by_hash) {
        ++per_seq[l->link.seq];
        for (const auto& r : l->link.returns) ++returned[r.hash];
    }
    for (const auto& [seq, n] : per_seq)
        if (n > 1) st.equivocated_seqs.push_back(seq);

    std::map<Hash32, bool> ok;
    for (const auto& [hash, l] : by_hash) {
        const LotLink& k = l->link;
        bool good = per_seq[k.seq] == 1;                  // one sale per remainder
        if (k.after > qty + EPS) good = false;             // more than the lot holds

        double back = 0;
        bool   returns_known = true;
        for (const auto& r : k.returns) {
            if (r.chain != chain) { good = false; continue; }
            const auto it = by_hash.find(r.hash);
            if (it == by_hash.end()) { returns_known = false; st.gaps = true; continue; }
            const ObservedLotLink& b = *it->second;
            if (!b.expires || *b.expires > l->timestamp || b.taken ||
                returned[r.hash] > 1 || b.link.seq >= k.seq)
                good = false;
            back += b.link.qty;
        }

        if (!k.prev) {
            if (std::abs(k.after - k.qty) > EPS) good = false;   // the first link: after = qty
        } else if (k.prev->chain != chain) {
            good = false;                                          // a sub-thread is one chain's
        } else if (const auto it = by_hash.find(k.prev->hash); it == by_hash.end()) {
            st.gaps = true;                                        // not received: a gap, not a fault
        } else {
            const LotLink& p = it->second->link;
            if (p.seq + 1 != k.seq) good = false;
            else if (returns_known && std::abs(p.after + k.qty - back - k.after) > EPS) good = false;
        }
        ok[hash] = good;
    }

    // Numbered units (ИР-023, вопрос 10): walking the sub-thread in order, a unit
    // is held by the link that took it until a later link returns that link. A
    // link taking a unit the lot has not, or one somebody holds, sells it twice;
    // a link of a numbered lot names its units, one of a lot by quantity names none.
    const std::set<std::string> lot_units(units.begin(), units.end());
    std::vector<const ObservedLotLink*> in_order;
    for (const auto& [hash, l] : by_hash) in_order.push_back(l);
    std::sort(in_order.begin(), in_order.end(),
              [](const ObservedLotLink* a, const ObservedLotLink* b) {
                  return a->link.seq != b->link.seq ? a->link.seq < b->link.seq
                                                    : a->hash < b->hash;
              });
    std::map<std::string, Hash32> holder;
    for (const auto* l : in_order) {
        if (!ok[l->hash]) continue;
        for (const auto& r : l->link.returns)
            for (auto it = holder.begin(); it != holder.end();)
                it = it->second == r.hash ? holder.erase(it) : std::next(it);
        bool good = lot_units.empty() == l->link.units.empty();
        for (const auto& u : l->link.units)
            if (!lot_units.count(u) || holder.count(u)) good = false;
        if (!good) { ok[l->hash] = false; continue; }
        for (const auto& u : l->link.units) holder[u] = l->hash;
    }
    for (const auto& [u, h] : holder) st.held.insert(u);
    for (const auto& [hash, good] : ok) (good ? st.valid : st.invalid).insert(hash);

    uint64_t top = 0;
    bool     any = false;
    for (const auto& hash : st.valid) {
        const LotLink& k = by_hash.at(hash)->link;
        if (!any || k.seq > top) { top = k.seq; st.used = k.after; any = true; }
    }
    if (any)
        for (uint64_t s = 0; s <= top; ++s)
            if (!per_seq.count(s)) { st.gaps = true; break; }
    return st;
}

std::optional<PortionTerms> portion_terms(const WorkRecord& wr, const LotLink& lot) {
    const auto f = portion_fraction(wr, lot);
    if (!f || wr.axes.empty()) return std::nullopt;
    const ResourceQty& o = wr.outputs[lot.out];
    if (!o.price) return std::nullopt;

    PortionTerms t;
    t.hours_raw   = wr.hours * *f;
    t.labor_units = *o.price * lot.qty / o.qty;
    double carried = 0.0;
    for (const auto& ce : wr.carry) carried += ce.carried;
    t.carried_units = carried * *f;
    std::vector<AxisShare> axes = wr.axes;
    std::sort(axes.begin(), axes.end(), [](const AxisShare& a, const AxisShare& b) {
        return a.axis.hash < b.axis.hash;
    });
    double left = t.labor_units;   // the last axis takes the remainder: Σ = price exactly
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const double u = i + 1 == axes.size() ? left : t.labor_units * axes[i].share;
        left -= u;
        t.axes.push_back({axes[i].axis, u});
    }
    return t;
}

bool meets_terms(const Acceptance& a, const PortionTerms& t) noexcept {
    constexpr double EPS = 1e-6;
    const double carried = a.carried_units ? *a.carried_units : 0.0;
    if (std::abs(a.hours_raw - t.hours_raw) > EPS || std::abs(a.labor_units - t.labor_units) > EPS ||
        std::abs(carried - t.carried_units) > EPS || a.axes.size() != t.axes.size())
        return false;
    for (const auto& x : t.axes) {
        const auto it = std::find_if(a.axes.begin(), a.axes.end(),
                                     [&](const AcceptanceAxis& y) { return y.axis == x.axis; });
        if (it == a.axes.end() || std::abs(it->units - x.units) > EPS) return false;
    }
    return true;
}

std::vector<HoldingLot> holding_lots(const Transfer& purchase, const WorkRecord& work,
                                     const Release* bought) {
    std::vector<HoldingLot> out;
    double paid = 0.0;
    for (const auto& o : purchase.origins) paid += o.units;
    if (bought && bought->lot) {
        const LotLink& l = *bought->lot;
        out.push_back({l.out, l.qty, l.units, paid});
        return out;
    }
    const std::size_t n = work.outputs.size();
    for (uint32_t i = 0; i < n; ++i) {
        const ResourceQty& o = work.outputs[i];
        const double share = o.share ? *o.share : (n == 1 ? 1.0 : 0.0);
        if (!(share > 0.0) || !(o.qty > 0.0)) continue;   // several outputs, no shares: not split
        out.push_back({i, o.qty, o.units, paid * share});
    }
    return out;
}

double resale_price(const HoldingLot& lot, double qty) noexcept {
    return lot.qty > 0.0 ? lot.cost * qty / lot.qty : 0.0;
}

bool meets_resale(const Acceptance& a, double price) noexcept {
    const double carried = a.carried_units ? *a.carried_units : 0.0;
    return a.labor_units == 0.0 && a.hours_raw == 0.0 && a.axes.empty() &&
           std::abs(carried - price) <= 1e-6;
}

} // namespace records
