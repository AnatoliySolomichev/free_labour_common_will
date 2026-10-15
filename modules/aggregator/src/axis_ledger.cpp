#include "aggregator/axis_ledger.h"
#include "aggregator/recognition.h"

#include <blockchain/crypto.h>
#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

namespace aggregator {

std::map<std::array<uint8_t, 32>, records::AxisDef> build_axis_definitions(
    const AggregatorStorage& storage) {
    // По хешу определяющего блока: этот блок И ЕСТЬ ось. Никакого поиска по
    // слагу — слаги сталкиваются, а выбирать между двумя цепями, назвавшими свою
    // ось «danger», значило бы чьё-то решение. Слаг — подпись для людей.
    std::map<std::array<uint8_t, 32>, records::AxisDef> out;
    for (const Hash& bh : storage.all_block_hashes()) {
        const SeenBlock& block = seen_block(storage, bh);   // decoded once per process
        if (!block.data) continue;
        if (const auto* d = std::get_if<records::AxisDef>(&block.record))
            out.emplace(bh.bytes, *d);
    }
    return out;
}

std::vector<AxisLedgerRow> build_axis_ledger(
    const AggregatorStorage&                                   storage,
    const std::map<std::array<uint8_t, 32>, records::AxisDef>* defs) {

    using RefHash = std::array<uint8_t, 32>;
    // Every deal is paid exactly by construction — the acceptance rides inside
    // its own payment (records.md §9.5, ИР-023).
    const auto  table = deals_on_table(storage);   // shared, recomputed only on new blocks
    const auto& deals = *table;

    struct Acc {
        double   units = 0.0, hours = 0.0;
        double   sum = 0.0, sumsq = 0.0;      // per-deal units/hours, hours-weighted
        uint64_t deals = 0;
        uint64_t broken = 0;
        std::set<RefHash> chains;
    };
    std::map<RefHash, Acc>     per_axis;
    std::map<RefHash, records::BlockPath> ident;

    // Путь обязан вести к блоку, названному хешем. Проверяется один раз на путь.
    using PathKey = std::tuple<RefHash, uint32_t, uint32_t, RefHash>;
    std::map<PathKey, bool> walked;
    auto path_leads = [&](const records::BlockPath& p) {
        const PathKey k{p.chain, p.node, p.block, p.hash};
        const auto it = walked.find(k);
        if (it != walked.end()) return it->second;
        UserId owner{};
        owner.bytes = p.chain;
        const auto at = storage.get_block(BlockAddress{owner, p.node, p.block});
        const bool ok = at && Crypto::hash_block(*at).bytes == p.hash;
        walked.emplace(k, ok);
        return ok;
    };
    double total_units = 0.0;

    // Одна работа — одна сделка (records.md §9.5, ИР-023): работа, оплаченная
    // двумя покупателями, продана дважды — в книгу не идёт ни одна.
    for (const auto& deal : deals.all) {
        if (!deals.counts(deal.hash)) continue;
        const auto& a = deal.accept();
        if (a.axes.empty() || a.hours_raw <= 0.0) continue;   // цена без объяснения

        // Разбивка ЕСТЬ цена, а не мнение рядом с ней.
        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (std::abs(named - a.labor_units) > 1e-6) continue;

        for (const auto& x : a.axes) {
            ident.emplace(x.axis.hash, x.axis);
            auto& acc = per_axis[x.axis.hash];
            if (!path_leads(x.axis)) acc.broken += 1;
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
    for (const auto& [hash, acc] : per_axis) {
        AxisLedgerRow r{};
        r.axis      = ident.at(hash);
        r.broken_paths = acc.broken;
        r.path_ok      = acc.broken == 0;
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
        if (defs) {
            const auto dit = defs->find(hash);
            if (dit != defs->end()) {
                r.label     = dit->second.ru;
                r.described = !dit->second.description.empty();
            }
        }
        out.push_back(std::move(r));
    }
    // Канонический порядок: по весу свидетельств, развязка по слагу — два
    // свидетеля обязаны напечатать одну таблицу из одних блоков.
    std::sort(out.begin(), out.end(), [](const AxisLedgerRow& a, const AxisLedgerRow& b) {
        if (a.units != b.units) return a.units > b.units;
        return a.axis.hash < b.axis.hash;
    });
    return out;
}

} // namespace aggregator
