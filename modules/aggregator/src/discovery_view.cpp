#include "aggregator/discovery_view.h"

#include <blockchain/errors.h>
#include <blockchain/serializer.h>
#include <records/codec.h>
#include <records/types.h>

#include <algorithm>

namespace aggregator {

DiscoveryView DiscoveryView::build(const AggregatorStorage& storage) {
    DiscoveryView view;

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block) continue;
        const UserId owner = block->address.user_id;
        view.chains_.insert(owner);

        if (block->type == BlockType::MERGE) {
            try {
                const MergePayload mp = Serializer::decode_merge_payload(
                    block->payload.data(), block->payload.size());
                const UserId partner = mp.partner_last_address.user_id;
                view.merges_[owner].insert(partner);
                view.merges_[partner].insert(owner);
                view.chains_.insert(partner);
            } catch (const SerializationError&) {}
            continue;
        }

        if (block->type != BlockType::DATA) continue;
        try {
            const auto rec = records::Codec::decode(block->payload.data(),
                                                    block->payload.size());
            const auto* t = std::get_if<records::Transfer>(&rec);
            if (!t || t->from != owner.bytes) continue;   // spoofed sender
            double total = 0;
            for (const auto& o : t->origins) total += o.units;
            UserId to{};
            to.bytes = t->to;
            view.econ_[owner][to] += total;
            view.econ_[to][owner] += total;
            view.chains_.insert(to);
        } catch (const records::CodecError&) {}
    }
    return view;
}

std::vector<DiscoveryCandidate>
DiscoveryView::candidates_for(const UserId& uid, std::size_t limit) const {
    // The subject's direct partners (economic or merge) — their partners are
    // the "neighbors" tier of sync.md §8.
    std::set<UserId> partners;
    if (auto e = econ_.find(uid); e != econ_.end())
        for (const auto& [chain, volume] : e->second) {
            (void)volume;
            partners.insert(chain);
        }
    if (auto m = merges_.find(uid); m != merges_.end())
        partners.insert(m->second.begin(), m->second.end());

    auto is_neighbor = [&](const UserId& c) {
        for (const UserId& p : partners) {
            if (auto m = merges_.find(p);
                m != merges_.end() && m->second.count(c)) return true;
            if (auto e = econ_.find(p);
                e != econ_.end() && e->second.count(c)) return true;
        }
        return false;
    };

    std::vector<DiscoveryCandidate> out;
    for (const UserId& chain : chains_) {
        if (chain == uid) continue;
        DiscoveryCandidate c;
        c.chain = chain;
        if (auto e = econ_.find(uid); e != econ_.end()) {
            auto v = e->second.find(chain);
            if (v != e->second.end()) c.econ_volume = v->second;
        }
        if (auto m = merges_.find(uid); m != merges_.end())
            c.merges_with = m->second.count(chain);
        if (auto m = merges_.find(chain); m != merges_.end())
            c.degree = m->second.size();
        c.neighbor = is_neighbor(chain);

        // §8 preference order as weights; a chain we already merged with
        // ranks lower — coverage grows faster through new partners.
        c.score = 3.0 * c.econ_volume
                + (c.neighbor ? 2.0 : 0.0)
                + 0.1 * static_cast<double>(c.degree);
        if (c.merges_with > 0) c.score *= 0.5;
        out.push_back(c);
    }

    std::sort(out.begin(), out.end(),
              [](const DiscoveryCandidate& a, const DiscoveryCandidate& b) {
                  if (a.score != b.score) return a.score > b.score;
                  return a.chain < b.chain;   // deterministic tie-break
              });
    if (out.size() > limit) out.resize(limit);
    return out;
}

} // namespace aggregator
