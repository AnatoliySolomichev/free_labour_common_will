#include "aggregator/rent_map.h"

#include <records/codec.h>

#include <cmath>

namespace aggregator {

std::map<std::pair<std::string, uint8_t>, BasketFlow> build_basket_flows(
    const AggregatorStorage& storage, int64_t from_ts, int64_t to_ts, double W) {

    using RefHash = std::array<uint8_t, 32>;
    std::map<RefHash, records::Record> by_hash;
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
        by_hash[bh.bytes] = std::move(rec);
    }

    const double norm = W > 0.0 ? W : 1.0;
    std::map<std::pair<std::string, uint8_t>, BasketFlow> out;
    for (const auto& [acc_hash, deal] : deals) {
        const auto& a = deal.acc;
        if (a.timestamp < from_ts || a.timestamp >= to_ts) continue;
        if (a.hours_raw <= 0.0) continue;
        const double carried = a.carried_units ? *a.carried_units : 0.0;
        const double payable = a.labor_units + carried;
        const auto pit = paid.find(acc_hash);
        if (pit == paid.end() || std::abs(pit->second - payable) > 1e-6) continue;
        if (a.work.chain == deal.payer.bytes) continue;                  // self-deal

        const auto wit = by_hash.find(a.work.hash);
        if (wit == by_hash.end()) continue;
        const auto* wr = std::get_if<records::WorkRecord>(&wit->second);
        if (!wr) continue;
        const auto git = by_hash.find(wr->agent.hash);
        if (git == by_hash.end()) continue;
        const auto* grade = std::get_if<records::Grade>(&git->second);
        if (!grade) continue;
        const auto sit = by_hash.find(grade->specialty.hash);
        if (sit == by_hash.end()) continue;
        const auto* spec = std::get_if<records::Specialty>(&sit->second);
        if (!spec) continue;

        const double rate   = a.labor_units / a.hours_raw / norm;
        const int64_t period = (a.timestamp - from_ts) / (kRentPeriodDays * 86'400);

        auto& b = out[{spec->name, grade->level}];
        auto& p = b.payers[deal.payer.bytes];
        p.hours      += a.hours_raw;
        p.rate_hours += rate * a.hours_raw;
        auto& q = b.periods[period];
        q.hours      += a.hours_raw;
        q.rate_hours += rate * a.hours_raw;
    }
    return out;
}

std::vector<RentRow> build_rent_map(
    const std::vector<AxisObservation>& obs,
    const std::vector<double>&          pred,
    const std::map<std::pair<std::string, uint8_t>, BasketFlow>& flows,
    unsigned min_payers) {

    std::vector<RentRow> out;
    out.reserve(obs.size());
    for (std::size_t i = 0; i < obs.size() && i < pred.size(); ++i) {
        const auto& o = obs[i];
        RentRow r{};
        r.slug  = o.slug;
        r.level = o.level;
        r.fact  = o.rate;
        r.pred  = pred[i];
        r.resid = o.rate - pred[i];
        r.hours = o.weight;

        const auto fit = flows.find({o.slug, o.level});
        if (fit == flows.end()) { out.push_back(std::move(r)); continue; }
        const auto& f = fit->second;
        r.payers = static_cast<unsigned>(f.payers.size());

        for (const auto& [idx, q] : f.periods) {
            (void)idx;
            if (q.hours <= 0.0) continue;
            ++r.periods_seen;
            if (q.rate_hours / q.hours > r.pred) ++r.periods_over;
        }

        // Concentration OF THE EXCESS, not of the volume. Only payers above the
        // model contribute: when everybody pays more, the model itself has
        // already absorbed it and there is no excess to concentrate.
        if (r.payers >= min_payers) {
            double total = 0.0;
            std::vector<double> exc;
            exc.reserve(f.payers.size());
            for (const auto& [who, p] : f.payers) {
                (void)who;
                if (p.hours <= 0.0) continue;
                const double over = p.rate_hours / p.hours - r.pred;
                if (over > 0.0) { exc.push_back(over * p.hours); total += over * p.hours; }
            }
            if (total > 0.0) {
                double hhi = 0.0;
                for (const double e : exc) { const double s = e / total; hhi += s * s; }
                r.rent_payers = hhi > 0.0 ? 1.0 / hhi : 0.0;
            }
            r.judged = true;
        }
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace aggregator
