#include "aggregator/rates_view.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

namespace aggregator {

namespace {

using RefHash = std::array<uint8_t, 32>;

struct Accum {
    double   w_units = 0;   // Σ weight·labor_units
    double   w_hours = 0;   // Σ weight·hours_raw — denominator of the average
    double   hours   = 0;   // Σ hours_raw, RAW: the labour did happen
    uint64_t deals   = 0;
};

// A deal whose whole provenance resolved (Acceptance → WorkRecord → Grade →
// Specialty) and which is exactly settled. `rate` is what this one deal paid
// per hour — the network rate times the k agreed in it (economy.md §2а).
struct ResolvedDeal {
    std::string specialty;
    uint8_t     level  = 0;
    RefHash     worker{};
    RefHash     payer{};
    double      units  = 0;
    double      hours  = 0;
    double      rate   = 0;
    bool        in_day = false;
};

// Unordered key for a pair of chains, so both directions land on one entry.
std::pair<RefHash, RefHash> pair_key(const RefHash& a, const RefHash& b) {
    return a < b ? std::make_pair(a, b) : std::make_pair(b, a);
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

// Prior for a thin, previously-unseen (specialty, level) bucket from the cloud
// (specialty-axes.md §10): weighted mean of neighbours that already have a rate at
// this level (a neighbour with none is "thin" and skipped) → tree parent's rate →
// 1.0. `prev0` is the immutable yesterday-rate map (not the one being drained).
static std::optional<double> cloud_prior(
    const std::string& specialty, uint8_t level,
    const records::SpecialtyCloud& cloud,
    const std::map<std::pair<std::string, uint8_t>, double>& prev0) {
    const records::CloudPoint* pt = nullptr;
    for (const auto& p : cloud.points)
        if (p.slug == specialty) { pt = &p; break; }
    if (!pt) return std::nullopt;
    double sw = 0.0, swr = 0.0;
    for (const auto& n : pt->neighbors) {
        const auto it = prev0.find({n.slug, level});
        if (it == prev0.end()) continue;          // neighbour has no rate yet → skip
        sw += n.weight;
        swr += n.weight * it->second;
    }
    if (sw > 0.0) return swr / sw;
    if (!pt->parent.empty()) {
        const auto it = prev0.find({pt->parent, level});
        if (it != prev0.end()) return it->second;
    }
    return 1.0;                                    // normalized par, last resort
}

std::vector<records::RateEntry> build_daily_rates(
    const AggregatorStorage&               storage,
    int64_t                                day_start,
    const std::vector<records::RateEntry>& previous,
    double                                 alpha,
    double                                 min_hours,
    const records::SpecialtyCloud*         cloud,
    const IndependenceParams*              indep) {
    const int64_t day_end = day_start + 86'400;
    // Without independence weighting only the day itself is ever looked at, so
    // the behaviour (and the cost of the scan) stays exactly as before.
    const bool    weighted = indep && indep->window_days > 0;
    const int64_t win_start =
        weighted ? day_start - indep->window_days * 86'400 : day_start;

    // One scan: records by block hash (for provenance resolution), the window's
    // acceptances, and the total paid per acceptance hash.
    std::map<RefHash, records::Record> by_hash;
    struct Deal {
        records::Acceptance acc;
        UserId              payer;
    };
    std::map<RefHash, Deal>   win_deals;
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
            if (a->timestamp >= win_start && a->timestamp < day_end)
                win_deals[bh.bytes] = Deal{*a, block->address.user_id};
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from == block->address.user_id.bytes && t->reason)
                for (const auto& o : t->origins)
                    paid[t->reason->hash] += o.units;
        }
        by_hash[bh.bytes] = std::move(rec);
    }

    // Resolve the window's settled, fully-resolvable, non-self deals.
    std::vector<ResolvedDeal> resolved;
    resolved.reserve(win_deals.size());
    for (const auto& [acc_hash, deal] : win_deals) {
        // Filter (Б) + records.md §12.8 "=": only exactly settled deals enter the
        // averaging — payment must equal the appraisal, an underpaid deal is
        // a hidden discount and must not move the rates (economy.md §4.2).
        // v2 (records.md §9.5): the appraisal is labor + carried cost of the means of
        // production; the averaging below still takes labor_units only.
        const double payable = deal.acc.labor_units
                             + (deal.acc.carried_units ? *deal.acc.carried_units
                                                       : 0.0);
        const auto pit = paid.find(acc_hash);
        if (pit == paid.end() ||
            std::abs(pit->second - payable) > 1e-6) continue;
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

        ResolvedDeal rd{};
        rd.specialty = spec->name;
        rd.level     = grade->level;
        rd.worker    = deal.acc.work.chain;
        rd.payer     = deal.payer.bytes;
        rd.units     = deal.acc.labor_units;
        rd.hours     = deal.acc.hours_raw;
        rd.rate      = rd.hours > 0.0 ? rd.units / rd.hours : 0.0;
        rd.in_day    = deal.acc.timestamp >= day_start;
        resolved.push_back(std::move(rd));
    }

    // ── ИР-021: counterparty independence over the window ────────────────────
    // Two ingredients, and the discount needs BOTH: how mutual the pair is, and
    // how far above its own basket the deal is priced.
    std::map<std::pair<RefHash, RefHash>, std::array<double, 2>> flow;
    std::map<std::pair<std::string, uint8_t>, std::vector<double>> basket;
    if (weighted) {
        for (const auto& d : resolved) {
            auto& f = flow[pair_key(d.payer, d.worker)];
            f[d.payer < d.worker ? 0 : 1] += d.units;
            if (d.rate > 0.0) basket[{d.specialty, d.level}].push_back(d.rate);
        }
        for (auto& [key, rates] : basket) std::sort(rates.begin(), rates.end());
    }

    // Weight of one deal: 1 − R·excess (see rates_view.h). Honest villages are
    // mutual but priced normally, so excess = 0 keeps their weight at 1.
    const auto weight_of = [&](const ResolvedDeal& d) -> double {
        if (!weighted) return 1.0;
        const auto fit = flow.find(pair_key(d.payer, d.worker));
        if (fit == flow.end()) return 1.0;
        const double ab = fit->second[0], ba = fit->second[1];
        const double gross = ab + ba;
        if (gross <= 0.0) return 1.0;
        const double R = 1.0 - std::abs(ab - ba) / gross;

        const auto bit = basket.find({d.specialty, d.level});
        if (bit == basket.end() || bit->second.empty()) return 1.0;
        const double median = quantile_sorted(bit->second, 0.5);
        if (median <= 0.0 || d.rate <= 0.0) return 1.0;

        // Self-calibrating threshold: "anomalous" relative to how wide the
        // bargaining in THIS basket actually is, not a global constant. Thin
        // baskets have no trustworthy spread, so they fall back to `kappa`.
        double kappa = indep->kappa;
        if (indep->self_calibrate &&
            bit->second.size() >= indep->calib_min_deals) {
            kappa = std::clamp(quantile_sorted(bit->second, 0.9) / median,
                               indep->kappa_min, indep->kappa_max);
        }
        if (kappa <= 1.0) return 1.0;

        const double anom   = d.rate / median;
        const double excess = std::clamp((anom - 1.0) / (kappa - 1.0), 0.0, 1.0);
        return 1.0 - R * excess;
    };

    // Fold the day's deals per (specialty, level).
    std::map<std::pair<std::string, uint8_t>, Accum> day;
    for (const auto& d : resolved) {
        if (!d.in_day) continue;
        const double w = weight_of(d);
        auto& acc = day[{d.specialty, d.level}];
        acc.w_units += w * d.units;
        acc.w_hours += w * d.hours;
        acc.hours   += d.hours;
        acc.deals   += 1;
    }

    // Smooth against yesterday; carry forward untouched specialties.
    std::map<std::pair<std::string, uint8_t>, double> prev_rate;
    for (const auto& p : previous) prev_rate[{p.specialty, p.level}] = p.rate;
    const std::map<std::pair<std::string, uint8_t>, double> prev0 = prev_rate;

    std::vector<records::RateEntry> out;
    for (const auto& [key, acc] : day) {
        records::RateEntry e{};
        e.specialty = key.first;
        e.level     = key.second;
        e.hours     = acc.hours;
        e.deals     = acc.deals;
        const auto prev = prev_rate.find(key);
        // Thinness is judged on the WEIGHTED volume: a bucket whose whole day
        // was discounted away carries no evidence, whatever its raw hours.
        if (acc.w_hours < min_hours) {
            if (prev != prev_rate.end()) {
                e.rate = prev->second;               // inherit unchanged
            } else if (cloud) {                      // seed a prior from the cloud
                const auto prior = cloud_prior(key.first, key.second, *cloud, prev0);
                if (!prior) continue;
                e.rate = *prior;
            } else {
                continue;                            // too little to seed a rate
            }
        } else {
            const double day_avg = acc.w_units / acc.w_hours;
            // Доверие ко дню = доля объёма, пережившая взвешивание. Само по себе
            // перевзвешивание не спасает корзину, где за день торговала ОДНА
            // сговорная пара: вес сокращается в числителе и знаменателе, среднее
            // не меняется. Поэтому вес управляет ещё и тем, насколько день
            // вправе двигать ставку: день, почти весь ушедший в дисконт,
            // оставляет вчерашнюю ставку почти нетронутой. При выключенном
            // взвешивании доверие ровно 1 и поведение прежнее.
            const double credibility =
                acc.hours > 0.0 ? std::clamp(acc.w_hours / acc.hours, 0.0, 1.0)
                                : 1.0;
            const double a = alpha * credibility;
            e.rate = prev == prev_rate.end()
                   ? day_avg
                   : a * day_avg + (1.0 - a) * prev->second;
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
