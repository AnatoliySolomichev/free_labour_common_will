#include "aggregator/rent_map.h"

#include <records/codec.h>

#include <cmath>
#include <set>
#include <utility>

namespace aggregator {

RentInput build_rent_input(const AggregatorStorage& storage,
                           int64_t from_ts, int64_t to_ts) {
    using RefHash = std::array<uint8_t, 32>;
    std::map<RefHash, records::WorkRecord> works;
    struct Deal { records::Acceptance acc; UserId payer; };
    std::map<RefHash, Deal>   deals;
    std::map<RefHash, double> paid;

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }
        if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            deals[bh.bytes] = Deal{*a, block->address.user_id};
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from == block->address.user_id.bytes && t->reason)
                for (const auto& o : t->origins) paid[t->reason->hash] += o.units;
        } else if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            works[bh.bytes] = *w;
        }
    }

    RentInput out;
    std::map<RefHash, double> axis_units, axis_hours;
    std::map<RefHash, std::pair<int64_t, RefHash>> titled;   // worker → (ts, acc) of its title
    for (const auto& [acc_hash, deal] : deals) {
        const auto& a = deal.acc;
        if (a.timestamp < from_ts || a.timestamp >= to_ts) continue;
        if (a.hours_raw <= 0.0 || a.axes.empty()) continue;
        const double carried = a.carried_units ? *a.carried_units : 0.0;
        const auto pit = paid.find(acc_hash);
        if (pit == paid.end() ||
            std::abs(pit->second - (a.labor_units + carried)) > 1e-6) continue;
        if (a.work.chain == deal.payer.bytes) continue;                  // self-deal

        // Разбивка ЕСТЬ цена: не сходящаяся описывает какую-то другую сделку.
        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (std::abs(named - a.labor_units) > 1e-6) continue;

        const auto wit = works.find(a.work.hash);
        if (wit == works.end()) continue;              // the work is not on the table

        DealTrace t{};
        t.payer  = deal.payer.bytes;
        t.hours  = a.hours_raw;
        t.units  = a.labor_units;
        t.period = (a.timestamp - from_ts) / (kRentPeriodDays * 86'400);
        for (const auto& x : a.axes) {
            t.axes[x.axis.hash]     += x.units;
            axis_units[x.axis.hash] += x.units;
            axis_hours[x.axis.hash] += a.hours_raw;
        }
        const RefHash worker = a.work.chain;
        out.workers[worker].push_back(std::move(t));

        if (!wit->second.title.empty()) {
            const auto when = std::make_pair(a.timestamp, acc_hash);
            const auto it   = titled.find(worker);
            if (it == titled.end() || it->second < when) {
                titled[worker]     = when;
                out.titles[worker] = wit->second.title;
            }
        }
    }
    for (const auto& [axis, u] : axis_units) {
        const double h = axis_hours[axis];
        out.per_hour[axis] = h > 0.0 ? u / h : 0.0;
    }
    return out;
}

std::vector<RentRow> build_rent_map(const RentInput& in, unsigned min_payers) {
    using RefHash = std::array<uint8_t, 32>;
    std::vector<RentRow> out;
    out.reserve(in.workers.size());
    for (const auto& [worker, deals] : in.workers) {
        RentRow r{};
        r.worker = worker;
        if (const auto it = in.titles.find(worker); it != in.titles.end())
            r.title = it->second;

        double units = 0.0, expected = 0.0;
        std::map<int64_t, std::pair<double, double>> by_period;   // units, expected
        std::map<RefHash, double> excess;                          // payer → excess
        std::set<RefHash> payers;
        for (const auto& t : deals) {
            // Ожидаемая цена — сетевая цена часа каждой названной оси; превышение
            // считается по осям, чтобы переплата за одну не пряталась за
            // недоплатой за другую.
            double exp_t = 0.0, over_t = 0.0;
            for (const auto& [axis, u] : t.axes) {
                const auto it   = in.per_hour.find(axis);
                const double nm = (it == in.per_hour.end() ? 0.0 : it->second) * t.hours;
                exp_t += nm;
                if (u > nm) over_t += u - nm;
            }
            r.hours  += t.hours;
            units    += t.units;
            expected += exp_t;
            auto& bp = by_period[t.period];
            bp.first  += t.units;
            bp.second += exp_t;
            payers.insert(t.payer);
            if (over_t > 0.0) excess[t.payer] += over_t;
        }
        if (r.hours > 0.0) {
            r.fact     = units / r.hours;
            r.expected = expected / r.hours;
            r.resid    = r.fact - r.expected;
        }
        r.payers = static_cast<unsigned>(payers.size());
        for (const auto& [idx, bp] : by_period) {
            (void)idx;
            ++r.periods_seen;
            if (bp.first > bp.second + 1e-9) ++r.periods_over;
        }

        // Концентрация ПРЕВЫШЕНИЯ, а не объёма. Когда дороже платят все, их
        // превышение размазано по всем плательщикам — это дефицит, а не труба.
        if (r.payers >= min_payers) {
            double total = 0.0;
            for (const auto& [who, e] : excess) { (void)who; total += e; }
            if (total > 0.0) {
                double hhi = 0.0;
                for (const auto& [who, e] : excess) {
                    (void)who;
                    const double s = e / total;
                    hhi += s * s;
                }
                r.rent_payers = hhi > 0.0 ? 1.0 / hhi : 0.0;
            }
            r.judged = true;
        }
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace aggregator
