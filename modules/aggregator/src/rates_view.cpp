#include "aggregator/rates_view.h"
#include "aggregator/recognition.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace aggregator {

namespace {

using RefHash = std::array<uint8_t, 32>;

// One axis's day: Σ weight·units on it, Σ weight·hours and raw hours of the
// work that named it.
struct AxisDay {
    records::BlockPath path;
    double   w_units = 0;
    double   w_hours = 0;
    double   hours   = 0;
    uint64_t deals   = 0;
};

// A deal whose provenance is on the table (the Acceptance and the WorkRecord it
// accepts) and which is exactly settled. `rate` is what this one deal paid per
// hour.
struct ResolvedDeal {
    RefHash     worker{};
    RefHash     payer{};
    double      units  = 0;
    double      hours  = 0;
    double      rate   = 0;
    bool        in_day = false;
    std::vector<records::AcceptanceAxis> axes;   // the breakdown
};

// Directed edge of the payment graph: who paid whom.
using Edge = std::pair<RefHash, RefHash>;

constexpr double kFlowEps = 1e-9;

// One cycle start→…→start of exactly `remaining`+path.size() edges, taking
// neighbours in canonical (sorted) order and never revisiting a node.
bool find_cycle(const std::map<Edge, double>& resid,
                const std::map<RefHash, std::vector<RefHash>>& out,
                const RefHash& start, const RefHash& node, size_t remaining,
                std::set<RefHash>& visited, std::vector<Edge>& path) {
    if (remaining == 1) {                       // last hop must close the ring
        const auto it = resid.find({node, start});
        if (it == resid.end() || it->second <= kFlowEps) return false;
        path.push_back({node, start});
        return true;
    }
    const auto oit = out.find(node);
    if (oit == out.end()) return false;
    for (const RefHash& nxt : oit->second) {
        if (nxt == start || visited.count(nxt)) continue;
        const auto it = resid.find({node, nxt});
        if (it == resid.end() || it->second <= kFlowEps) continue;
        visited.insert(nxt);
        path.push_back({node, nxt});
        if (find_cycle(resid, out, start, nxt, remaining - 1, visited, path))
            return true;
        path.pop_back();
        visited.erase(nxt);
    }
    return false;
}

// How much of each edge's flow returns through a cycle of length ≤ max_len.
// Greedy cancelling, shortest cycles first. Greedy decomposition depends on the
// order cycles are cancelled in, so the order is fixed and derivable from the
// data alone (std::map iterates sorted; adjacency lists inherit that order) —
// otherwise two witnesses would compute different weights from the same blocks.
std::map<Edge, double> circulation(std::map<Edge, double> resid,
                                   size_t max_len) {
    std::map<Edge, double> circ;
    std::map<RefHash, std::vector<RefHash>> out;
    std::vector<Edge> order;
    for (const auto& [e, v] : resid) {
        circ[e] = 0.0;
        out[e.first].push_back(e.second);
        order.push_back(e);
    }
    for (size_t len = 2; len <= max_len; ++len) {
        for (const Edge& e0 : order) {
            while (resid[e0] > kFlowEps) {
                std::set<RefHash> visited{e0.first, e0.second};
                std::vector<Edge>  path{e0};
                if (!find_cycle(resid, out, e0.first, e0.second, len - 1,
                                visited, path))
                    break;
                double m = resid[path.front()];
                for (const Edge& e : path) m = std::min(m, resid[e]);
                for (const Edge& e : path) { resid[e] -= m; circ[e] += m; }
            }
        }
    }
    return circ;
}

// Linear-interpolated quantile of an already sorted, non-empty sample.
double quantile_sorted(const std::vector<double>& v, double q) {
    if (v.empty()) return 0.0;
    if (v.size() == 1) return v.front();
    const double pos  = q * static_cast<double>(v.size() - 1);
    const size_t lo   = static_cast<size_t>(std::floor(pos));
    const size_t hi   = static_cast<size_t>(std::ceil(pos));
    const double frac = pos - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

} // namespace

DailyPrices build_daily_prices(
    const AggregatorStorage&               storage,
    int64_t                                day_start,
    const std::vector<records::AxisPrice>& previous,
    double                                 alpha,
    double                                 min_hours,
    const IndependenceParams*              indep) {
    const int64_t day_end = day_start + 86'400;
    // Without independence weighting only the day itself is ever looked at, so
    // the behaviour (and the cost of the scan) stays exactly as before.
    const bool    weighted = indep && indep->window_days > 0;
    const int64_t win_start =
        weighted ? day_start - indep->window_days * 86'400 : day_start;

    // The window's deals whose work is on the table. Filter (Б) and the «=» of
    // records.md §12.8 hold by construction: the acceptance rides inside its own
    // payment (records.md §9.5, ИР-023), so every deal is settled exactly and is
    // never a self-deal. The payment is labor + carried cost of the means of
    // production (v2); the averaging below takes labor_units only. One work —
    // one deal: a work paid by two buyers was sold twice and moves no price.
    const auto  table = deals_on_table(storage);   // shared, recomputed only on new blocks
    const auto& deals = *table;
    // The WorkRecords on the table (for provenance resolution) are the table's
    // own — no second scan of the storage.
    std::vector<ResolvedDeal> resolved;
    for (const auto& deal : deals.all) {
        const int64_t ts = deal.transfer.timestamp;
        if (ts < win_start || ts >= day_end) continue;
        if (!deals.counts(deal.hash)) continue;
        const auto& a = deal.accept();
        // Разбивка ЕСТЬ цена (records.md §9.5 v4): не сходящаяся описывает
        // другую сделку — как в книге осей, её нет ни в ставке, ни в опоре.
        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (a.axes.empty() || std::abs(named - a.labor_units) > 1e-6) continue;
        if (!deals.works.count(a.work.hash)) continue;   // the work is not on the table

        ResolvedDeal rd{};
        rd.worker    = a.work.chain;
        rd.payer     = deal.payer.bytes;
        rd.units     = a.labor_units;
        rd.hours     = a.hours_raw;
        rd.rate      = rd.hours > 0.0 ? rd.units / rd.hours : 0.0;
        rd.in_day    = ts >= day_start;
        rd.axes      = a.axes;
        resolved.push_back(std::move(rd));
    }

    // ── ИР-021: counterparty independence over the window ────────────────────
    // Two ingredients, and the discount needs BOTH: how mutual the pair is, and
    // how far the deal is priced above the network's price of what it named.
    //
    // The reference is the network's own axis prices (option «б», chosen
    // 2026-09-11): per axis, Σ units on it / Σ hours of the work that named it,
    // over every settled deal of the window. A deal's expected price per hour is
    // the sum over the axes it named — the rent map's reference (records.md
    // §11.11). A premium NAMED in the breakdown is priced by everybody who names
    // that axis; a premium hidden in the axes everybody names is an anomaly.
    //
    // An axis's price counts only once `min_axis_payers` DISTINCT payers have
    // named it: an axis is worth what independent people pay for it (ИР-022,
    // strength from use). Until then pay on it is unexplained — not forbidden,
    // discounted only where the flow also circulates. Otherwise a pair would
    // invent an axis of its own and price its inflation with it.
    std::map<Edge, double> flow, circ;
    std::map<RefHash, double> known;              // axis → network price per hour
    double kappa = weighted ? indep->kappa : 0.0;
    const auto expected_of = [&](const ResolvedDeal& d) {
        double e = 0.0;
        std::set<RefHash> seen;
        for (const auto& x : d.axes) {
            if (!seen.insert(x.axis.hash).second) continue;
            const auto it = known.find(x.axis.hash);
            if (it != known.end()) e += it->second;
        }
        return e;
    };
    if (weighted) {
        struct AxisPool {
            double            units = 0.0, hours = 0.0;
            std::set<RefHash> payers;
        };
        std::map<RefHash, AxisPool> pools;
        for (const auto& d : resolved) {
            flow[{d.payer, d.worker}] += d.units;
            std::set<RefHash> seen;
            for (const auto& x : d.axes) {
                auto& p = pools[x.axis.hash];
                p.units += x.units;
                if (seen.insert(x.axis.hash).second) {   // an hour counts once per axis
                    p.hours += d.hours;
                    p.payers.insert(d.payer);
                }
            }
        }
        for (const auto& [axis, p] : pools)
            if (p.payers.size() >= indep->min_axis_payers && p.hours > 0.0)
                known[axis] = p.units / p.hours;
        circ = circulation(flow, std::max<size_t>(2, indep->max_cycle));

        // Self-calibrating threshold: "anomalous" relative to how wide the
        // bargaining in the network actually is, not a global constant. Too few
        // judgeable deals — the fallback `kappa`.
        if (indep->self_calibrate) {
            std::vector<double> anoms;
            for (const auto& d : resolved) {
                const double e = expected_of(d);
                if (e > 0.0 && d.rate > 0.0) anoms.push_back(d.rate / e);
            }
            if (anoms.size() >= indep->calib_min_deals) {
                std::sort(anoms.begin(), anoms.end());
                kappa = std::clamp(quantile_sorted(anoms, 0.9),
                                   indep->kappa_min, indep->kappa_max);
            }
        }
    }

    // Weight of one deal: 1 − R·excess (see rates_view.h). Honest villages are
    // mutual but priced like the network, so excess = 0 keeps their weight at 1.
    const auto weight_of = [&](const ResolvedDeal& d) -> double {
        if (!weighted) return 1.0;
        const auto fit = flow.find({d.payer, d.worker});
        const auto cit = circ.find({d.payer, d.worker});
        if (fit == flow.end() || cit == circ.end() ||
            fit->second <= 0.0) return 1.0;
        const double R = std::clamp(cit->second / fit->second, 0.0, 1.0);
        if (d.rate <= 0.0 || kappa <= 1.0) return 1.0;

        // Nothing it named has a price yet — nothing to compare with. Refuse to
        // judge rather than punish an honest young kind of work.
        const double e = expected_of(d);
        if (e <= 0.0) return 1.0;

        const double anom   = d.rate / e;
        const double excess = std::clamp((anom - 1.0) / (kappa - 1.0), 0.0, 1.0);
        return 1.0 - R * excess;
    };

    // Fold the day's deals per axis. A deal's hours count once for every axis it
    // named — an hour of that work paid for each of them — and its units on an
    // axis go to that axis alone.
    std::map<RefHash, AxisDay> day;
    double day_hours = 0.0;
    for (const auto& d : resolved) {
        if (!d.in_day) continue;
        const double w = weight_of(d);
        day_hours += d.hours;
        std::set<RefHash> seen;
        for (const auto& x : d.axes) {
            const auto [it, fresh] = day.try_emplace(x.axis.hash);
            auto& a = it->second;
            if (fresh) a.path = x.axis;
            a.w_units += w * x.units;
            if (seen.insert(x.axis.hash).second) {
                a.w_hours += w * d.hours;
                a.hours   += d.hours;
                a.deals   += 1;
            }
        }
    }

    // Smooth against yesterday; carry forward untouched axes.
    std::map<RefHash, records::AxisPrice> prev;
    for (const auto& p : previous) prev[p.axis.hash] = p;

    DailyPrices out;
    for (const auto& [key, a] : day) {
        records::AxisPrice e{};
        e.axis           = a.path;
        e.hours          = a.hours;
        e.deals          = a.deals;
        e.weighted_hours = a.w_hours;   // ИР-021: сколько объёма уцелело
        const auto pit = prev.find(key);
        // Thinness is judged on the WEIGHTED volume: an axis whose whole day was
        // discounted away carries no evidence, whatever its raw hours.
        if (a.w_hours < min_hours) {
            if (pit == prev.end()) continue;          // nobody has paid for it yet
            e.price = pit->second.price;              // inherit unchanged
        } else {
            const double day_avg = a.w_units / a.w_hours;
            // Доверие ко дню = доля объёма, пережившая взвешивание. Само по себе
            // перевзвешивание не спасает ось, где за день торговала ОДНА
            // сговорная пара: вес сокращается в числителе и знаменателе, среднее
            // не меняется. Поэтому вес управляет ещё и тем, насколько день
            // вправе двигать цену: день, почти весь ушедший в дисконт, оставляет
            // вчерашнюю цену почти нетронутой. При выключенном взвешивании
            // доверие ровно 1 и сглаживание обычное.
            const double credibility =
                a.hours > 0.0 ? std::clamp(a.w_hours / a.hours, 0.0, 1.0) : 1.0;
            const double al = alpha * credibility;
            e.price = pit == prev.end()
                    ? day_avg
                    : al * day_avg + (1.0 - al) * pit->second.price;
        }
        out.axes.push_back(std::move(e));
        if (pit != prev.end()) prev.erase(pit);
    }
    for (const auto& [key, p] : prev) {
        (void)key;
        out.axes.push_back({p.axis, p.price, 0.0, 0, 0.0});
    }
    std::sort(out.axes.begin(), out.axes.end(),
              [](const records::AxisPrice& a, const records::AxisPrice& b) {
                  return a.axis.hash < b.axis.hash;
              });

    // W (economy.md §2б). An axis too thin to be priced does not count.
    double paid_out = 0.0;
    for (const auto& p : out.axes) paid_out += p.price * p.hours;
    out.W = day_hours > 0.0 ? paid_out / day_hours : 1.0;
    return out;
}

} // namespace aggregator
