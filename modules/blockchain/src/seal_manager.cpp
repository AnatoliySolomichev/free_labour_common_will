#include "blockchain/seal_manager.h"
#include "blockchain/errors.h"

namespace blockchain {

SealManager::SealManager(IStorage& storage) : storage_(storage) {}

Seal SealManager::create_seal(const Hash&, const KeyPair&, SealMode) {
    throw BlockchainError("SealManager::create_seal: not implemented");
}

Seal SealManager::create_open_seal(const Block&, const KeyPair&) {
    throw BlockchainError("SealManager::create_open_seal: not implemented");
}

std::vector<Seal> SealManager::get_seals(const Hash& block_hash) const {
    return storage_.get_seals(block_hash);
}

std::optional<Timestamp> SealManager::compute_witnessed_time(const BlockAddress&) const {
    return std::nullopt;
}

} // namespace blockchain
