#include "aggregator/attestation_view.h"
#include "aggregator/recognition.h"

#include <records/codec.h>

#include <algorithm>
#include <cmath>
#include <set>

namespace aggregator {

namespace {

using RefHash = std::array<uint8_t, 32>;

struct Accept {
    RefHash             hash{};       // the acceptance block
    UserId              acceptor{};
    records::Acceptance acc;
};

} // namespace

AttestationView AttestationView::build(const AggregatorStorage& storage) {
    AttestationView view;

    // One work — one recognized acceptance (records.md §9.5, ИР-023): a job
    // accepted by five chains is one job, not «приняли пять цепей».
    const auto recognition = recognize_acceptances(storage);

    std::map<RefHash, UserId> works;     // WorkRecord hash → the chain that signed it
    std::vector<Accept>       accepts;

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        const UserId owner = block->address.user_id;

        records::Record rec;
        try {
            rec = records::Codec::decode(block->payload.data(),
                                         block->payload.size());
        } catch (const records::CodecError&) {
            continue;
        }

        if (std::holds_alternative<records::WorkRecord>(rec)) {
            works[bh.bytes] = owner;
        } else if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            if (a->receiver != owner.bytes) continue;   // spoofed receiver
            accepts.push_back(Accept{bh.bytes, owner, *a});
        }
    }

    std::map<std::pair<UserId, RefHash>, std::set<UserId>> witnesses;
    std::map<UserId, std::set<UserId>>                     all_witnesses;
    for (const auto& [hash, acceptor, a] : accepts) {
        if (!recognition.recognized.count(hash)) continue;   // not the one that counts
        const auto work = works.find(a.work.hash);
        if (work == works.end()) continue;
        const UserId& worker = work->second;
        if (a.work.chain != worker.bytes) continue;     // the Ref lies about the chain
        if (acceptor == worker) continue;               // the free self-vouch

        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (std::abs(named - a.labor_units) > 1e-6) continue;   // another deal

        std::set<RefHash> seen;
        for (const auto& x : a.axes) {
            if (!seen.insert(x.axis.hash).second) continue;
            const auto [it, fresh] = view.axes_.try_emplace({worker, x.axis.hash});
            if (fresh) it->second.axis = x.axis;
            it->second.hours += a.hours_raw;
            witnesses[{worker, x.axis.hash}].insert(acceptor);
        }
        view.totals_[worker].hours += a.hours_raw;
        all_witnesses[worker].insert(acceptor);
    }
    for (const auto& [key, who] : witnesses) view.axes_[key].chains = who.size();
    for (const auto& [w, who] : all_witnesses) view.totals_[w].chains = who.size();

    return view;
}

std::vector<AttestedAxis> AttestationView::of(const UserId& worker) const {
    std::vector<AttestedAxis> out;
    for (auto it = axes_.lower_bound({worker, RefHash{}});
         it != axes_.end() && it->first.first == worker; ++it)
        out.push_back(it->second);
    std::sort(out.begin(), out.end(), [](const AttestedAxis& a, const AttestedAxis& b) {
        if (a.hours != b.hours) return a.hours > b.hours;
        return a.axis.hash < b.axis.hash;
    });
    return out;
}

std::optional<AttestedAxis> AttestationView::at(
        const UserId& worker, const std::array<uint8_t, 32>& axis_hash) const {
    const auto it = axes_.find({worker, axis_hash});
    if (it == axes_.end()) return std::nullopt;
    return it->second;
}

AttestedTotal AttestationView::total(const UserId& worker) const {
    const auto it = totals_.find(worker);
    return it == totals_.end() ? AttestedTotal{} : it->second;
}

} // namespace aggregator
