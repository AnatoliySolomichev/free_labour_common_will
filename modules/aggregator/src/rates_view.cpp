#include "aggregator/rates_view.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace aggregator {

namespace {

using RefHash = std::array<uint8_t, 32>;

struct Accum {
    double   units = 0;
    double   hours = 0;
    uint64_t deals = 0;
};

} // namespace

std::vector<records::RateEntry> build_daily_rates(
    const AggregatorStorage&               storage,
    int64_t                                day_start,
    const std::vector<records::RateEntry>& previous,
    double                                 alpha,
    double                                 min_hours) {
    const int64_t day_end = day_start + 86'400;

    // One scan: records by block hash (for provenance resolution), the day's
    // acceptances, and the total paid per acceptance hash.
    std::map<RefHash, records::Record> by_hash;
    struct Deal {
        records::Acceptance acc;
        UserId              payer;
    };
    std::map<RefHash, Deal>   day_deals;
    std::map<RefHash, double> paid;

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        records::Record rec;
        try {
            rec = records::Codec::decode(block->payload.data(),
                                         block->payload.size());
        } catch (const records::CodecError&) {
            continue;
        }

        if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            if (a->timestamp >= day_start && a->timestamp < day_end)
                day_deals[bh.bytes] = Deal{*a, block->address.user_id};
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from == block->address.user_id.bytes && t->reason)
                for (const auto& o : t->origins)
                    paid[t->reason->hash] += o.units;
        }
        by_hash[bh.bytes] = std::move(rec);
    }

    // Fold settled, fully-resolvable, non-self deals per (specialty, level).
    std::map<std::pair<std::string, uint8_t>, Accum> day;
    for (const auto& [acc_hash, deal] : day_deals) {
        // Filter (Б) + §12.8 "=": only exactly settled deals enter the
        // averaging — payment must equal the appraisal, an underpaid deal is
        // a hidden discount and must not move the rates (economy.md §4.2).
        const auto pit = paid.find(acc_hash);
        if (pit == paid.end() ||
            std::abs(pit->second - deal.acc.labor_units) > 1e-6) continue;
        if (deal.acc.work.chain == deal.payer.bytes) continue;     // self-deal

        const auto work_it = by_hash.find(deal.acc.work.hash);
        if (work_it == by_hash.end()) continue;
        const auto* wr = std::get_if<records::WorkRecord>(&work_it->second);
        if (!wr) continue;
        const auto grade_it = by_hash.find(wr->agent.hash);
        if (grade_it == by_hash.end()) continue;
        const auto* grade = std::get_if<records::Grade>(&grade_it->second);
        if (!grade) continue;
        const auto spec_it = by_hash.find(grade->specialty.hash);
        if (spec_it == by_hash.end()) continue;
        const auto* spec = std::get_if<records::Specialty>(&spec_it->second);
        if (!spec) continue;

        auto& acc = day[{spec->name, grade->level}];
        acc.units += deal.acc.labor_units;
        acc.hours += deal.acc.hours_raw;
        acc.deals += 1;
    }

    // Smooth against yesterday; carry forward untouched specialties.
    std::map<std::pair<std::string, uint8_t>, double> prev_rate;
    for (const auto& p : previous) prev_rate[{p.specialty, p.level}] = p.rate;

    std::vector<records::RateEntry> out;
    for (const auto& [key, acc] : day) {
        records::RateEntry e{};
        e.specialty = key.first;
        e.level     = key.second;
        e.hours     = acc.hours;
        e.deals     = acc.deals;
        const auto prev = prev_rate.find(key);
        if (acc.hours < min_hours) {
            if (prev == prev_rate.end()) continue;   // too little to seed a rate
            e.rate = prev->second;                   // inherit unchanged
        } else {
            const double day_avg = acc.units / acc.hours;
            e.rate = prev == prev_rate.end()
                   ? day_avg
                   : alpha * day_avg + (1.0 - alpha) * prev->second;
        }
        out.push_back(std::move(e));
        prev_rate.erase(key);
    }
    for (const auto& [key, rate] : prev_rate)
        out.push_back({key.first, key.second, rate, 0.0, 0});

    std::sort(out.begin(), out.end(),
              [](const records::RateEntry& a, const records::RateEntry& b) {
                  return a.specialty != b.specialty ? a.specialty < b.specialty
                                                    : a.level < b.level;
              });
    return out;
}

} // namespace aggregator
