#include "aggregator/recognition.h"

#include <records/codec.h>

namespace aggregator {

DealsOnTable read_deals(const AggregatorStorage& storage) {
    DealsOnTable out;
    std::vector<records::DealClaim> claims;
    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }

        const auto* t = std::get_if<records::Transfer>(&rec);
        if (!t || !t->accept) continue;
        if (t->from != block->address.user_id.bytes) continue;   // spoofed sender
        claims.push_back({bh.bytes, t->accept->work.hash});
        out.all.push_back(DealEntry{bh.bytes, block->address.user_id, *t});
    }
    out.recognition = records::recognize_deals(claims);
    return out;
}

} // namespace aggregator
