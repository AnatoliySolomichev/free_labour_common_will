#include "blockchain/fraud.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/key_path.h"

namespace blockchain {
namespace {

// Verify block.signature against working_pubkey (canonical convention: encode with
// the signature field zeroed).
bool block_sig_valid(const Block& block, const PublicKey& working_pubkey) {
    Block tv = block;
    tv.signature = Signature::null();
    auto bytes = Serializer::encode(tv);
    return Crypto::verify(bytes.data(), bytes.size(), block.signature, working_pubkey);
}

// Shared structural check: Merkle inclusion of the leaf + a valid key path of
// the leaf's chain to its node (blockchain.md §7.5). Returns false → fabricated;
// on true, sets working_pubkey to the node's key at the leaf's place. A path
// that does not check throws — the callers count that as fabricated too.
bool structural_ok(const Hash& merkle_root, const FraudProofData& d,
                   PublicKey& working_pubkey) {
    Hash lh = MerkleTree::leaf_hash(d.leaf);
    if (!MerkleTree::verify(lh, d.merkle_path, merkle_root)) return false;
    if (!(KeyPathCert::verify(d.path) == d.leaf.address.user_id)) return false;
    if (KeyPathCert::node(d.path) != d.leaf.address.node_index) return false;
    working_pubkey = KeyPathCert::key_at(d.path, d.leaf.address.node_index,
                                         d.leaf.address.block_index);
    return true;
}

} // namespace (anonymous)

FraudVerdict FraudProof::verify_bad_sig(const Hash& merkle_root,
                                        const FraudProofData& d) noexcept {
    try {
        PublicKey wpk;
        if (!structural_ok(merkle_root, d, wpk))
            return FraudVerdict::REFUTED_FABRICATED;
        // evidence must BE the committed block.
        if (!(d.evidence.address == d.leaf.address))
            return FraudVerdict::REFUTED_FABRICATED;
        if (Crypto::hash_block(d.evidence) != d.leaf.block_hash)
            return FraudVerdict::REFUTED_FABRICATED;
        // Defect: is the committed block's signature actually invalid?
        return block_sig_valid(d.evidence, wpk)
                 ? FraudVerdict::REFUTED_HONEST   // signature valid → no fraud
                 : FraudVerdict::CONFIRMED;       // signature invalid → fraud proven
    } catch (...) {
        return FraudVerdict::REFUTED_FABRICATED;
    }
}

FraudVerdict FraudProof::verify_hash_mismatch(const Hash& merkle_root,
                                              const FraudProofData& d) noexcept {
    try {
        PublicKey wpk;
        if (!structural_ok(merkle_root, d, wpk))
            return FraudVerdict::REFUTED_FABRICATED;
        // Counter-evidence must sit at the committed leaf's address.
        if (!(d.evidence.address == d.leaf.address))
            return FraudVerdict::REFUTED_FABRICATED;
        // It must be a real, validly-signed block — else it proves nothing.
        if (!block_sig_valid(d.evidence, wpk))
            return FraudVerdict::REFUTED_FABRICATED;
        // Defect: the committed hash differs from the real block's hash.
        return (Crypto::hash_block(d.evidence) != d.leaf.block_hash)
                 ? FraudVerdict::CONFIRMED        // committed a wrong hash → fraud proven
                 : FraudVerdict::REFUTED_HONEST;  // hashes match → no mismatch
    } catch (...) {
        return FraudVerdict::REFUTED_FABRICATED;
    }
}

FraudVerdict FraudProof::verify(const std::string& kind,
                                const uint8_t* proof, size_t len,
                                const Hash& merkle_root) noexcept {
    try {
        FraudProofData d = Serializer::decode_fraud_proof(proof, len);
        if (kind == "bad_sig")       return verify_bad_sig(merkle_root, d);
        if (kind == "hash_mismatch") return verify_hash_mismatch(merkle_root, d);
        return FraudVerdict::REFUTED_FABRICATED;   // unknown kind
    } catch (...) {
        return FraudVerdict::REFUTED_FABRICATED;
    }
}

} // namespace blockchain
