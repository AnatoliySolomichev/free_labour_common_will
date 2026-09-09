#include "aggregator/axis_ledger.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace aggregator {

std::vector<AxisLedgerRow> build_axis_ledger(
    const AggregatorStorage&             storage,
    const std::vector<records::Catalog>* catalogs) {

    using RefHash = std::array<uint8_t, 32>;
    std::map<RefHash, records::Record> by_hash;
    struct Deal { records::Acceptance acc; UserId payer; };
    std::map<RefHash, Deal>   deals;
    std::map<RefHash, double> paid;
    struct Said { records::DealProfile prof; UserId author; };
    std::vector<Said> said;

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
        } else if (const auto* p = std::get_if<records::DealProfile>(&rec)) {
            said.push_back({*p, block->address.user_id});
        }
        by_hash[bh.bytes] = std::move(rec);
    }

    struct Acc {
        double   units = 0.0, hours = 0.0;
        double   sum = 0.0, sumsq = 0.0;      // per-deal units/hours, hours-weighted
        uint64_t deals = 0;
        std::set<RefHash> chains;
    };
    std::map<std::string, Acc> per_axis;
    double total_units = 0.0;

    // One breakdown per (deal, author): a later profile from the same party
    // supersedes their earlier one, so nobody votes twice by rewriting.
    std::map<std::pair<RefHash, RefHash>, const records::DealProfile*> latest;
    for (const auto& sp : said) {
        auto& slot = latest[{sp.prof.deal.hash, sp.author.bytes}];
        if (!slot || sp.prof.timestamp >= slot->timestamp) slot = &sp.prof;
    }

    for (const auto& [key, prof] : latest) {
        const auto dit = deals.find(key.first);
        if (dit == deals.end()) continue;
        const auto& a = dit->second.acc;
        if (a.hours_raw <= 0.0) continue;

        const double carried = a.carried_units ? *a.carried_units : 0.0;
        const auto pit = paid.find(key.first);
        if (pit == paid.end() ||
            std::abs(pit->second - (a.labor_units + carried)) > 1e-6) continue;
        if (a.work.chain == dit->second.payer.bytes) continue;          // self-deal
        const bool party = key.second == dit->second.payer.bytes
                        || key.second == a.work.chain;
        if (!party) continue;

        // The breakdown must BE the payment, not sit beside it.
        double named = 0.0;
        for (const auto& ax : prof->axes) named += ax.units;
        if (std::abs(named - a.labor_units) > 1e-6) continue;

        for (const auto& ax : prof->axes) {
            auto& acc = per_axis[ax.axis];
            const double per_hour = ax.units / a.hours_raw;
            acc.units += ax.units;
            acc.hours += a.hours_raw;
            acc.sum   += a.hours_raw * per_hour;
            acc.sumsq += a.hours_raw * per_hour * per_hour;
            acc.deals += 1;
            acc.chains.insert(key.second);
            total_units += ax.units;
        }
    }

    std::set<std::string> described;
    if (catalogs)
        for (const auto& cat : *catalogs)
            for (const auto& e : cat.entries)
                if (!e.ru.empty()) described.insert(e.slug);

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
        r.described = described.count(slug) != 0;
        out.push_back(std::move(r));
    }
    // Canonical: by weight of evidence, ties by slug — two witnesses must print
    // the same table from the same blocks.
    std::sort(out.begin(), out.end(), [](const AxisLedgerRow& a, const AxisLedgerRow& b) {
        if (a.units != b.units) return a.units > b.units;
        return a.slug < b.slug;
    });
    return out;
}

} // namespace aggregator
