#include "blockchain/fraud.h"
#include "blockchain/blockchain.h"
#include "blockchain/crypto.h"
#include "blockchain/merkle.h"
#include "blockchain/serializer.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <map>

using namespace blockchain;

static constexpr NodeIndex LEAF = 0x7FFF'FFFFu;

static Hash make_hash(uint8_t fill) { Hash h; h.bytes.fill(fill); return h; }

// Builds one participant chain with a signed block-0 at LEAF, and exposes the
// node path + block used to construct fraud proofs.
class FraudTest : public ::testing::Test {
protected:
    std::filesystem::path        db_;
    std::unique_ptr<LmdbStorage> storage_;
    std::unique_ptr<Validator>   validator_;
    std::unique_ptr<Blockchain>  bc_;

    KeyPair                      root_kp_;
    KeyPair                      leaf_kp_;
    std::map<NodeIndex, KeyPair> keys_;
    Block                        block_;   // signed block-0
    std::vector<Node>            path_;    // root..leaf nodes

    void SetUp() override {
        static int cnt = 0;
        db_ = std::filesystem::temp_directory_path() / ("bc_fraud_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_);
        storage_   = std::make_unique<LmdbStorage>(db_);
        validator_ = std::make_unique<Validator>(*storage_);
        bc_        = std::make_unique<Blockchain>(*storage_, *validator_);

        root_kp_ = Crypto::generate_keypair();
        keys_[0] = root_kp_;
        bc_->create_identity(root_kp_);
        for (NodeIndex idx : path_indices(LEAF))
            if (keys_.find(idx) == keys_.end()) keys_[idx] = Crypto::generate_keypair();
        bc_->ensure_path(root_kp_.pub, LEAF, [&](NodeIndex i) { return keys_.at(i); });
        leaf_kp_ = keys_.at(LEAF);

        block_ = bc_->append_data_block(root_kp_.pub, LEAF, {0x01, 0x02}, leaf_kp_, 1'000LL);
        path_  = bc_->get_path(root_kp_.pub, LEAF);
    }

    void TearDown() override {
        bc_.reset(); validator_.reset(); storage_.reset();
        std::filesystem::remove_all(db_);
    }

    // Committed leaf for block_ with an arbitrary committed hash.
    ExternalRef ref_with_hash(const Hash& h) const { return ExternalRef{block_.address, h}; }

    // Single-leaf snapshot: root == leaf_hash(leaf), empty inclusion proof.
    static Hash single_root(const ExternalRef& leaf) { return MerkleTree::leaf_hash(leaf); }

    Block corrupt_sig(Block b) const { b.signature.bytes[0] ^= 0xFF; return b; }
};

// ── bad_sig ───────────────────────────────────────────────────────────────────

TEST_F(FraudTest, BadSigConfirmed) {
    Block bad = corrupt_sig(block_);
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(bad));  // leaf commits the bad block
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, bad};
    EXPECT_EQ(FraudProof::verify_bad_sig(single_root(leaf), d), FraudVerdict::CONFIRMED);
}

TEST_F(FraudTest, BadSigRefutedHonestWhenSignatureValid) {
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(block_));
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, block_};
    EXPECT_EQ(FraudProof::verify_bad_sig(single_root(leaf), d), FraudVerdict::REFUTED_HONEST);
}

TEST_F(FraudTest, BadSigFabricatedWrongRoot) {
    Block bad = corrupt_sig(block_);
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(bad));
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, bad};
    // Root does not match leaf_hash(leaf) → inclusion fails.
    EXPECT_EQ(FraudProof::verify_bad_sig(make_hash(0x00), d), FraudVerdict::REFUTED_FABRICATED);
}

TEST_F(FraudTest, BadSigFabricatedBrokenNodePath) {
    Block bad = corrupt_sig(block_);
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(bad));
    std::vector<Node> broken = path_;
    broken[0].parent_sig.bytes[0] ^= 0xFF;   // corrupt root self-signature
    FraudProofData d{leaf, MerkleTree::Proof{}, broken, bad};
    EXPECT_EQ(FraudProof::verify_bad_sig(single_root(leaf), d), FraudVerdict::REFUTED_FABRICATED);
}

TEST_F(FraudTest, BadSigFabricatedEvidenceHashMismatch) {
    // evidence is not the committed block (leaf.hash points elsewhere).
    Block bad = corrupt_sig(block_);
    ExternalRef leaf = ref_with_hash(make_hash(0xEE));   // committed hash ≠ hash(bad)
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, bad};
    EXPECT_EQ(FraudProof::verify_bad_sig(single_root(leaf), d), FraudVerdict::REFUTED_FABRICATED);
}

TEST_F(FraudTest, BadSigConfirmedWithNonEmptyMerklePath) {
    Block bad = corrupt_sig(block_);
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(bad));

    // Two-leaf snapshot so the inclusion proof carries a real sibling.
    Hash lh    = MerkleTree::leaf_hash(leaf);
    Hash other = MerkleTree::leaf_hash(ref_with_hash(make_hash(0x99)));
    std::vector<Hash> leaves{lh, other};
    Hash root  = MerkleTree::root(leaves);
    auto proof = MerkleTree::make_proof(leaves, 0);

    FraudProofData d{leaf, proof, path_, bad};
    EXPECT_EQ(FraudProof::verify_bad_sig(root, d), FraudVerdict::CONFIRMED);
}

// ── hash_mismatch ─────────────────────────────────────────────────────────────

TEST_F(FraudTest, HashMismatchConfirmed) {
    ExternalRef leaf = ref_with_hash(make_hash(0xEE));   // committed a wrong hash
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, block_};  // real block as counter
    EXPECT_EQ(FraudProof::verify_hash_mismatch(single_root(leaf), d), FraudVerdict::CONFIRMED);
}

TEST_F(FraudTest, HashMismatchRefutedHonestWhenHashesMatch) {
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(block_));  // committed the correct hash
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, block_};
    EXPECT_EQ(FraudProof::verify_hash_mismatch(single_root(leaf), d), FraudVerdict::REFUTED_HONEST);
}

TEST_F(FraudTest, HashMismatchFabricatedWhenCounterUnsigned) {
    ExternalRef leaf = ref_with_hash(make_hash(0xEE));
    Block bad_counter = corrupt_sig(block_);   // invalid signature → not a real block
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, bad_counter};
    EXPECT_EQ(FraudProof::verify_hash_mismatch(single_root(leaf), d),
              FraudVerdict::REFUTED_FABRICATED);
}

// ── proof serialization + verify-by-kind (records connector) ──────────────────

TEST_F(FraudTest, ProofDataSerializationRoundTrip) {
    Block bad = corrupt_sig(block_);
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(bad));

    Hash lh    = MerkleTree::leaf_hash(leaf);
    Hash other = MerkleTree::leaf_hash(ref_with_hash(make_hash(0x99)));
    std::vector<Hash> leaves{lh, other};
    auto proof = MerkleTree::make_proof(leaves, 0);

    FraudProofData d{leaf, proof, path_, bad};
    auto enc = Serializer::encode(d);
    FraudProofData d2 = Serializer::decode_fraud_proof(enc.data(), enc.size());

    EXPECT_EQ(d2.leaf,        d.leaf);
    EXPECT_EQ(d2.merkle_path, d.merkle_path);
    ASSERT_EQ(d2.node_path.size(), d.node_path.size());
    EXPECT_EQ(d2.evidence.address,   d.evidence.address);
    EXPECT_EQ(d2.evidence.signature, d.evidence.signature);

    // The decoded proof yields the same verdict as the original.
    EXPECT_EQ(FraudProof::verify_bad_sig(MerkleTree::root(leaves), d2),
              FraudVerdict::CONFIRMED);
}

TEST_F(FraudTest, VerifyByKindDispatch) {
    Block bad = corrupt_sig(block_);
    ExternalRef leaf = ref_with_hash(Crypto::hash_block(bad));
    FraudProofData d{leaf, MerkleTree::Proof{}, path_, bad};
    auto bytes = Serializer::encode(d);
    Hash root  = single_root(leaf);

    EXPECT_EQ(FraudProof::verify("bad_sig", bytes.data(), bytes.size(), root),
              FraudVerdict::CONFIRMED);
    EXPECT_EQ(FraudProof::verify("nonsense", bytes.data(), bytes.size(), root),
              FraudVerdict::REFUTED_FABRICATED);   // unknown kind

    const std::vector<uint8_t> junk{0x00, 0x01, 0x02};
    EXPECT_EQ(FraudProof::verify("bad_sig", junk.data(), junk.size(), root),
              FraudVerdict::REFUTED_FABRICATED);    // malformed bytes
}
