#include "blockchain/crypto.h"
#include "blockchain/errors.h"
#include "blockchain/serializer.h"
#include <sodium.h>

namespace {

// Ensure libsodium is initialised exactly once per process.
// abort() is intentional: failure here means the platform is fundamentally broken.
struct SodiumGuard {
    SodiumGuard() {
        if (sodium_init() < 0) {
            abort();
        }
    }
};

const SodiumGuard sodium_guard;

} // namespace

namespace blockchain {

KeyPair Crypto::generate_keypair() {
    KeyPair kp;
    static_assert(sizeof(kp.pub.bytes) == crypto_sign_PUBLICKEYBYTES);
    static_assert(sizeof(kp.sec.bytes) == crypto_sign_SECRETKEYBYTES);
    if (crypto_sign_keypair(kp.pub.bytes.data(), kp.sec.bytes.data()) != 0) {
        throw CryptoError("Ed25519 keypair generation failed");
    }
    return kp;
}

Hash Crypto::hash(const uint8_t* data, size_t len) noexcept {
    Hash h;
    static_assert(sizeof(h.bytes) == crypto_generichash_BYTES);
    crypto_generichash(h.bytes.data(), h.bytes.size(), data, len, nullptr, 0);
    return h;
}

Signature Crypto::sign(const uint8_t* data, size_t len, const SecretKey& key) {
    Signature sig;
    static_assert(sizeof(sig.bytes) == crypto_sign_BYTES);
    unsigned long long siglen = 0;
    if (crypto_sign_detached(sig.bytes.data(), &siglen, data, len, key.bytes.data()) != 0) {
        throw CryptoError("Ed25519 signing failed");
    }
    return sig;
}

bool Crypto::verify(const uint8_t* data, size_t len,
                    const Signature& sig, const PublicKey& key) noexcept {
    return crypto_sign_verify_detached(
               sig.bytes.data(), data, len, key.bytes.data()) == 0;
}

Hash Crypto::hash_node(const Node& node) {
    auto encoded = Serializer::encode(node);
    return hash(encoded.data(), encoded.size());
}

Hash Crypto::hash_block(const Block& block) {
    auto encoded = Serializer::encode(block);
    return hash(encoded.data(), encoded.size());
}

} // namespace blockchain
