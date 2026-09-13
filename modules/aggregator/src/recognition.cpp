#include "aggregator/recognition.h"

#include <records/codec.h>

#include <map>
#include <vector>

namespace aggregator {

records::AcceptanceRecognition recognize_acceptances(const AggregatorStorage& storage) {
    using RefHash = std::array<uint8_t, 32>;
    struct Seen { RefHash work{}; double payable = 0.0; };
    std::map<RefHash, Seen>   accs;   // acceptance hash → what it appraises
    std::map<RefHash, double> paid;   // acceptance hash → Σ transfers naming it

    for (const Hash& bh : storage.all_block_hashes()) {
        const auto block = storage.get_block_by_hash(bh);
        if (!block || block->type != BlockType::DATA) continue;
        const UserId owner = block->address.user_id;
        records::Record rec;
        try { rec = records::Codec::decode(block->payload.data(), block->payload.size()); }
        catch (const records::CodecError&) { continue; }

        if (const auto* a = std::get_if<records::Acceptance>(&rec)) {
            if (a->receiver != owner.bytes) continue;     // spoofed receiver
            if (a->work.chain == owner.bytes) continue;   // self-acceptance vouches for nothing
            accs[bh.bytes] = Seen{a->work.hash,
                                  a->labor_units + (a->carried_units ? *a->carried_units : 0.0)};
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from != owner.bytes || !t->reason) continue;   // spoofed sender
            for (const auto& o : t->origins) paid[t->reason->hash] += o.units;
        }
    }

    std::vector<records::AcceptanceClaim> claims;
    claims.reserve(accs.size());
    for (const auto& [hash, seen] : accs) {
        const auto pit = paid.find(hash);
        claims.push_back({hash, seen.work,
                          records::settled_exactly(pit == paid.end() ? 0.0 : pit->second,
                                                   seen.payable)});
    }
    return records::recognize_acceptances(claims);
}

} // namespace aggregator
