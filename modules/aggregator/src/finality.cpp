#include "aggregator/finality.h"

#include <blockchain/crypto.h>
#include <blockchain/revocation.h>
#include <blockchain/serializer.h>

#include <algorithm>

namespace aggregator {

namespace {

bool signed_by(const Block& block, const PublicKey& key) {
    Block to_verify     = block;
    to_verify.signature = Signature::null();
    const auto bytes    = Serializer::encode(to_verify);
    return Crypto::verify(bytes.data(), bytes.size(), block.signature, key);
}

// The revocations that decide the branch: the highest author's certificates
// from the warehouse, each checked here, in the author's block order — and the
// earliest moment one of them came here.
struct Decided {
    std::vector<RevocationPayload> records;
    std::optional<Timestamp>       came;
};

Decided deciding_revocations(const AggregatorStorage& storage, const UserId& chain,
                             NodeIndex node) {
    struct Cert {
        uint32_t                 depth;
        BlockIndex               block;
        RevocationPayload        payload;
        std::optional<Timestamp> came;
    };
    std::vector<Cert> certs;
    for (const auto& bytes : storage.get_revocation_bytes(chain)) {
        try {
            const auto cert = Serializer::decode_revocation_cert(bytes.data(), bytes.size());
            RevocationCert::verify(cert);
            const auto p = RevocationCert::payload(cert);
            if (p.revoked_node_index != node) continue;
            certs.push_back({node_depth(cert.block.address.node_index),
                             cert.block.address.block_index, p,
                             storage.block_arrival(Crypto::hash_block(cert.block))});
        } catch (const std::exception&) {}   // a certificate that does not check: none
    }
    Decided out;
    if (certs.empty()) return out;
    const uint32_t top = std::min_element(certs.begin(), certs.end(),
                                          [](const Cert& a, const Cert& b) {
                                              return a.depth < b.depth;
                                          })->depth;
    std::sort(certs.begin(), certs.end(),
              [](const Cert& a, const Cert& b) { return a.block < b.block; });
    for (const Cert& c : certs) {
        if (c.depth != top) continue;          // the higher ancestor decided
        out.records.push_back(c.payload);
        if (c.came && (!out.came || *c.came < *out.came)) out.came = c.came;
    }
    return out;
}

// Whether `b` lies on the owner's line ending at `line`: prev_hash back
// through the blocks kept here, down to b's index.
bool on_line(const AggregatorStorage& storage, const Block& b, const LastGood& line) {
    if (b.address.block_index > line.index) return false;
    Hash at = line.hash;
    for (BlockIndex i = line.index; i > b.address.block_index; --i) {
        const auto step = storage.get_block_by_hash(at);
        if (!step || step->address.node_index != b.address.node_index ||
            step->address.block_index != i)
            return false;
        at = step->prev_hash;
    }
    return at == Crypto::hash_block(b);
}

} // namespace

const char* finality_name(Finality f) noexcept {
    switch (f) {
        case Finality::pending: return "pending";
        case Finality::final:   return "final";
        case Finality::voided:  return "voided";
        case Finality::frozen:  return "frozen";
    }
    return "pending";
}

DealFinality deal_finality(const AggregatorStorage& storage, const Block& offer,
                           Timestamp answered, int64_t window, Timestamp now) {
    DealFinality out;
    out.final_at          = answered + window;
    const UserId&   chain = offer.address.user_id;
    const NodeIndex node  = offer.address.node_index;

    const Decided rev = deciding_revocations(storage, chain, node);
    std::optional<LastGood> line;
    for (const auto& p : rev.records)
        if (p.last_good) line = p.last_good;      // the latest word on the line
    // The owner's: signed by a replacement a revocation named, or on the line.
    const bool owners =
        std::any_of(rev.records.begin(), rev.records.end(),
                    [&](const RevocationPayload& p) {
                        return p.replacement_pubkey && signed_by(offer, *p.replacement_pubkey);
                    })
        || (line && on_line(storage, offer, *line));
    if (!rev.records.empty() && rev.came && *rev.came <= out.final_at && !owners) {
        out.state      = Finality::voided;
        out.revoked_at = rev.came;
        return out;
    }
    // A fork of the paying branch that came within the window, its line not named.
    if (!line)
        for (const auto& f : storage.forks(chain)) {
            if (f.address.node_index != node) continue;
            const auto came = storage.block_arrival(f.rival);
            if (!came || *came <= out.final_at) {
                out.state = Finality::frozen;
                return out;
            }
        }
    out.state = now >= out.final_at ? Finality::final : Finality::pending;
    return out;
}

} // namespace aggregator
