#include "blockchain/seal_manager.h"
#include "blockchain/crypto.h"
#include "blockchain/errors.h"
#include "blockchain/serializer.h"

namespace blockchain {

SealManager::SealManager(IStorage& storage) : storage_(storage) {}

Seal SealManager::create_seal(const Hash& block_hash,
                               const KeyPair& signer_keypair,
                               SealMode mode) {
    Seal s{};
    s.signer_id  = signer_keypair.pub;
    s.block_hash = block_hash;
    s.mode       = mode;
    s.sealed_at  = 0; // no timestamp parameter in this interface
    s.signature  = Crypto::sign(block_hash.bytes.data(),
                                block_hash.bytes.size(),
                                signer_keypair.sec);
    storage_.put_seal(s);
    return s;
}

Seal SealManager::create_open_seal(const Block& block,
                                    const KeyPair& signer_keypair) {
    Hash bh = Crypto::hash_block(block);
    Seal s{};
    s.signer_id  = signer_keypair.pub;
    s.block_hash = bh;
    s.mode       = SealMode::OPEN;
    s.sealed_at  = 0;
    s.signature  = Crypto::sign(bh.bytes.data(), bh.bytes.size(), signer_keypair.sec);
    storage_.put_seal(s);
    return s;
}

std::vector<Seal> SealManager::get_seals(const Hash& block_hash) const {
    return storage_.get_seals(block_hash);
}

std::optional<Timestamp> SealManager::compute_witnessed_time(
    const BlockAddress& address) const
{
    // Scan external blocks (other users' blocks imported during merges) for MERGE
    // blocks that reference our branch at index >= address.block_index.
    // The earliest such timestamp is an upper bound: our block existed before it.
    std::optional<Timestamp> best;

    storage_.for_each_external_block([&](const Block& b) -> bool {
        if (b.type != BlockType::MERGE) return true;
        try {
            const MergePayload mp = Serializer::decode_merge_payload(
                b.payload.data(), b.payload.size());
            if (mp.partner_last_address.user_id    == address.user_id   &&
                mp.partner_last_address.node_index == address.node_index &&
                mp.partner_last_address.block_index >= address.block_index)
            {
                if (!best || b.timestamp_claimed < *best)
                    best = b.timestamp_claimed;
            }
        } catch (const SerializationError&) {}
        return true;
    });

    return best;
}

} // namespace blockchain
