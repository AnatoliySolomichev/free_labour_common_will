#pragma once

#include "storage.h"
#include <optional>
#include <vector>

namespace blockchain {

class SealManager {
public:
    explicit SealManager(IStorage& storage);

    // Create and store a BLIND seal (signs only block_hash).
    // Throws: CryptoError, StorageError.
    Seal create_seal(const Hash& block_hash,
                     const KeyPair& signer_keypair,
                     SealMode mode);

    // Create and store an OPEN seal (verifies block_hash matches block first).
    // Throws: CryptoError, StorageError, InvalidArgumentError (hash mismatch).
    Seal create_open_seal(const Block& block, const KeyPair& signer_keypair);

    std::vector<Seal> get_seals(const Hash& block_hash) const;

    // Compute witnessed_time: the latest upper bound on when the block existed.
    // Derived from MERGE blocks that reference this block. Returns nullopt if no
    // external witnesses are known.
    // [OPEN §11.3] gossip propagation of revocations not implemented in MVP.
    std::optional<Timestamp> compute_witnessed_time(const BlockAddress& address) const;

private:
    IStorage& storage_;
};

} // namespace blockchain
