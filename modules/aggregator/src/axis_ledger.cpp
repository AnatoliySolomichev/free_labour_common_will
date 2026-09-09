#include "aggregator/axis_ledger.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace aggregator {

std::map<std::string, records::AxisDef> build_axis_definitions(
    const AggregatorStorage& storage) {
    struct Seen { records::AxisDef def; std::array<uint8_t, 32> chain; };
    std::map<std::string, Seen> out;
    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }
        const auto* d = std::get_if<records::AxisDef>(&rec);
        if (!d || d->slug.empty()) continue;
        const auto it = out.find(d->slug);
        // Первое по (времени, цепи) — определение слага; спор о слаге это факт о
        // нём, а не то, что здесь надо разрешать.
        if (it == out.end() ||
            std::make_pair(d->timestamp, block->address.user_id.bytes) <
            std::make_pair(it->second.def.timestamp, it->second.chain))
            out[d->slug] = Seen{*d, block->address.user_id.bytes};
    }
    std::map<std::string, records::AxisDef> defs;
    for (auto& [slug, seen] : out) defs.emplace(slug, std::move(seen.def));
    return defs;
}

std::vector<AxisLedgerRow> build_axis_ledger(
    const AggregatorStorage&                       storage,
    const std::map<std::string, records::AxisDef>* defs) {

    using RefHash = std::array<uint8_t, 32>;
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
        }
    }

    struct Acc {
        double   units = 0.0, hours = 0.0;
        double   sum = 0.0, sumsq = 0.0;      // per-deal units/hours, hours-weighted
        uint64_t deals = 0;
        std::set<RefHash> chains;
    };
    std::map<std::string, Acc> per_axis;
    double total_units = 0.0;

    for (const auto& [acc_hash, deal] : deals) {
        const auto& a = deal.acc;
        if (a.axes.empty() || a.hours_raw <= 0.0) continue;   // цена без объяснения

        const double carried = a.carried_units ? *a.carried_units : 0.0;
        const auto pit = paid.find(acc_hash);
        if (pit == paid.end() ||
            std::abs(pit->second - (a.labor_units + carried)) > 1e-6) continue;
        if (a.work.chain == deal.payer.bytes) continue;                 // самосделка

        // Разбивка ЕСТЬ цена, а не мнение рядом с ней.
        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (std::abs(named - a.labor_units) > 1e-6) continue;

        for (const auto& x : a.axes) {
            auto& acc = per_axis[x.axis];
            const double per_hour = x.units / a.hours_raw;
            acc.units += x.units;
            acc.hours += a.hours_raw;
            acc.sum   += a.hours_raw * per_hour;
            acc.sumsq += a.hours_raw * per_hour * per_hour;
            acc.deals += 1;
            acc.chains.insert(deal.payer.bytes);
            total_units += x.units;
        }
    }

    std::vector<AxisLedgerRow> out;
    out.reserve(per_axis.size());
    for (const auto& [slug, acc] : per_axis) {
        AxisLedgerRow r{};
        r.slug      = slug;
        r.units     = acc.units;
        r.hours     = acc.hours;
        r.deals     = acc.deals;
        r.chains    = static_cast<uint64_t>(acc.chains.size());
        r.share     = total_units > 0.0 ? acc.units / total_units : 0.0;
        r.per_hour  = acc.hours > 0.0 ? acc.units / acc.hours : 0.0;
        if (acc.hours > 0.0) {
            const double mean = acc.sum / acc.hours;
            r.spread = std::sqrt(std::max(0.0, acc.sumsq / acc.hours - mean * mean));
        }
        r.described = defs && defs->count(slug) != 0
                   && !defs->at(slug).description.empty();
        out.push_back(std::move(r));
    }
    // Канонический порядок: по весу свидетельств, развязка по слагу — два
    // свидетеля обязаны напечатать одну таблицу из одних блоков.
    std::sort(out.begin(), out.end(), [](const AxisLedgerRow& a, const AxisLedgerRow& b) {
        if (a.units != b.units) return a.units > b.units;
        return a.slug < b.slug;
    });
    return out;
}

} // namespace aggregator
