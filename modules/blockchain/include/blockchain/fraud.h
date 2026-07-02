#pragma once

#include "types.h"
#include "merkle.h"
#include <string>
#include <vector>

namespace blockchain {

// Verdict of checking a FraudClaim proof (records.md §3A.1, blockchain.md §6.5.6).
enum class FraudVerdict {
    CONFIRMED,           // fraud proven → strong negative on the accused
    REFUTED_HONEST,      // structurally sound but the defect is absent → zero weight, no penalty
    REFUTED_FABRICATED,  // proof itself is bogus (bad Merkle/node path) → penalty to the accuser
};

// Self-verifying inputs of a FraudClaim proof. The same shape serves both kinds;
// only the interpretation of `evidence` differs. The accused merge block's
// committed merkle_root is passed to the verify functions separately.
struct FraudProofData {
    ExternalRef       leaf;         // disputed participant: (chain, node, block, committed hash)
    MerkleTree::Proof merkle_path;  // inclusion of leaf_hash(leaf) under merkle_root
    std::vector<Node> node_path;    // nodes root..leaf.node of leaf.chain (→ working_pubkey)
    Block             evidence;     // bad_sig: the committed block; hash_mismatch: the real block
};

// Stateless verifier. Both functions are total: any malformed or bogus proof
// yields REFUTED_FABRICATED rather than throwing.
class FraudProof {
public:
    FraudProof() = delete;

    // "bad_sig": the committed leaf references a block-0 with an invalid signature.
    // evidence must be that committed block (hash == leaf.block_hash).
    static FraudVerdict verify_bad_sig(const Hash& merkle_root,
                                       const FraudProofData& data) noexcept;

    // "hash_mismatch": the committed leaf hash disagrees with the participant's
    // real, validly-signed block at the same address (carried in evidence).
    static FraudVerdict verify_hash_mismatch(const Hash& merkle_root,
                                             const FraudProofData& data) noexcept;

    // End-to-end entry: decode a serialized proof (the opaque `proof` bytes of a
    // records::FraudClaim) and dispatch on `kind`. Unknown kind or malformed bytes
    // → REFUTED_FABRICATED. `merkle_root` is the accused merge block's commitment.
    static FraudVerdict verify(const std::string& kind,
                               const uint8_t* proof, size_t len,
                               const Hash& merkle_root) noexcept;
};

} // namespace blockchain
