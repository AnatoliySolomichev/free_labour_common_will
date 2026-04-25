#pragma once

#include "types.h"

namespace blockchain {

// Wrapper around libsodium. All methods are static; no state.
class Crypto {
public:
    Crypto() = delete;

    // Generate a new Ed25519 key pair. Throws: CryptoError.
    static KeyPair generate_keypair();

    // BLAKE2b-256 hash of arbitrary data.
    static Hash hash(const uint8_t* data, size_t len) noexcept;

    // Ed25519 signature. Throws: CryptoError.
    static Signature sign(const uint8_t* data, size_t len, const SecretKey& key);

    // Ed25519 signature verification. Returns false on invalid signature (no throw).
    static bool verify(const uint8_t* data, size_t len,
                       const Signature& sig, const PublicKey& key) noexcept;

    // Hash of a node's canonical CBOR encoding (used as parent_hash in children).
    // Throws: SerializationError, CryptoError.
    static Hash hash_node(const Node& node);

    // Hash of a block's canonical CBOR encoding (co_signature field excluded).
    // Throws: SerializationError, CryptoError.
    static Hash hash_block(const Block& block);
};

} // namespace blockchain
