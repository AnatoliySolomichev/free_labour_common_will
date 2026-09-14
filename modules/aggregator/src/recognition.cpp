#include "aggregator/recognition.h"

#include <records/codec.h>

namespace aggregator {

DealsOnTable read_deals(const AggregatorStorage& storage) {
    DealsOnTable out;
    std::map<std::array<uint8_t, 32>, std::size_t> release_at;   // release hash → index
    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        const UserId owner = block->address.user_id;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }

        if (const auto* w = std::get_if<records::WorkRecord>(&rec)) {
            out.works[bh.bytes] = WorkOnTable{owner, w->customer};
        } else if (const auto* rl = std::get_if<records::Release>(&rec)) {
            if (rl->work.chain != owner.bytes) continue;   // only one's own work is released
            release_at[bh.bytes] = out.releases.size();
            out.releases.push_back(ReleaseEntry{bh.bytes, owner, *rl});
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (!t->accept) continue;
            if (t->from != owner.bytes) continue;          // spoofed sender
            out.all.push_back(DealEntry{bh.bytes, owner, *t});
        }
    }

    // Blocks come in hash order, so the seller's records are matched to the
    // deals only after the whole scan.
    std::vector<records::DealClaim> claims;
    claims.reserve(out.all.size());
    for (const auto& d : out.all) {
        const records::Acceptance& a = d.accept();
        std::optional<std::array<uint8_t, 32>> customer;
        if (const auto w = out.works.find(a.work.hash);
            w != out.works.end() && w->second.worker.bytes == a.work.chain)
            customer = w->second.customer;
        const records::Release* release = nullptr;
        if (a.release)
            if (const auto r = release_at.find(a.release->hash);
                r != release_at.end() && out.releases[r->second].seller.bytes == a.release->chain)
                release = &out.releases[r->second].release;
        claims.push_back({d.hash, a.work.hash, d.payer.bytes, d.transfer.timestamp,
                          records::addressed_to(d.transfer, customer, release)});
    }
    out.recognition = records::recognize_deals(claims);
    return out;
}

} // namespace aggregator
