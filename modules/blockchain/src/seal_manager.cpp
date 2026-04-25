#include "blockchain/seal_manager.h"
#include "blockchain/crypto.h"
#include "blockchain/errors.h"

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
    const BlockAddress&) const
{
    // [MVP] Requires a block-reference index to scan MERGE blocks efficiently.
    // Not implemented until such an index exists in storage.
    return std::nullopt;
}

} // namespace blockchain
