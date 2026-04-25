#include "blockchain/validator.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/storage.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace blockchain;

// ── Signing helpers (mirror the production logic) ─────────────────────────────

static Node make_signed_root(const KeyPair& kp, Timestamp ts = 1'000'000LL) {
    Node n{};
    n.index             = 0;
    n.structural_pubkey = kp.pub;
    n.working_pubkey    = kp.pub;
    n.parent_hash       = Hash::zero();
    n.created_at        = ts;
    n.parent_sig        = Signature::null();
    auto b              = Serializer::encode(n);
    n.parent_sig        = Crypto::sign(b.data(), b.size(), kp.sec);
    return n;
}

static Node make_signed_child(NodeIndex idx, const KeyPair& kp,
                               const Node& parent, const KeyPair& parent_kp,
                               Timestamp ts = 1'000'001LL) {
    Node n{};
    n.index             = idx;
    n.structural_pubkey = kp.pub;
    n.working_pubkey    = kp.pub;
    n.parent_hash       = Crypto::hash_node(parent);
    n.created_at        = ts;
    n.parent_sig        = Signature::null();
    auto b              = Serializer::encode(n);
    n.parent_sig        = Crypto::sign(b.data(), b.size(), parent_kp.sec);
    return n;
}

static Block make_signed_block(const BlockAddress& addr, const Hash& prev_hash,
                                Timestamp ts, BlockType type,
                                const std::vector<uint8_t>& payload,
                                const KeyPair& kp) {
    Block b{};
    b.address           = addr;
    b.prev_hash         = prev_hash;
    b.timestamp_claimed = ts;
    b.type              = type;
    b.payload           = payload;
    b.signature         = Signature::null();
    auto bytes          = Serializer::encode(b);
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

// ── Fixture ───────────────────────────────────────────────────────────────────

class ValidatorTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<LmdbStorage> storage_;
    std::unique_ptr<Validator>   validator_;

    KeyPair root_kp_;
    KeyPair child1_kp_; // node index 1
    KeyPair child3_kp_; // node index 3 (left child of 1)
    Node    root_, child1_, child3_;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_val_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_   = std::make_unique<LmdbStorage>(db_path_);
        validator_ = std::make_unique<Validator>(*storage_);

        root_kp_   = Crypto::generate_keypair();
        child1_kp_ = Crypto::generate_keypair();
        child3_kp_ = Crypto::generate_keypair();

        root_   = make_signed_root(root_kp_);
        child1_ = make_signed_child(1, child1_kp_, root_,   root_kp_);
        child3_ = make_signed_child(3, child3_kp_, child1_, child1_kp_);

        storage_->put_node(root_kp_.pub, root_);
        storage_->put_node(root_kp_.pub, child1_);
        storage_->put_node(root_kp_.pub, child3_);
    }

    void TearDown() override {
        validator_.reset();
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }
};

// ── validate_node ─────────────────────────────────────────────────────────────

TEST_F(ValidatorTest, ValidateRootOk) {
    EXPECT_NO_THROW(validator_->validate_node(root_, root_kp_.pub));
}

TEST_F(ValidatorTest, ValidateChildOk) {
    EXPECT_NO_THROW(validator_->validate_node(child1_, root_kp_.pub));
}

TEST_F(ValidatorTest, ValidateGrandchildOk) {
    EXPECT_NO_THROW(validator_->validate_node(child3_, root_kp_.pub));
}

TEST_F(ValidatorTest, ValidateRootBadSig) {
    Node bad = root_;
    bad.parent_sig.bytes[0] ^= 0xFF; // corrupt
    EXPECT_THROW(validator_->validate_node(bad, root_kp_.pub), SignatureError);
}

TEST_F(ValidatorTest, ValidateChildBadSig) {
    Node bad = child1_;
    bad.parent_sig.bytes[5] ^= 0xFF;
    EXPECT_THROW(validator_->validate_node(bad, root_kp_.pub), SignatureError);
}

TEST_F(ValidatorTest, ValidateRootNonZeroParentHash) {
    Node bad = root_;
    bad.parent_hash.bytes[0] = 0xAB;
    EXPECT_THROW(validator_->validate_node(bad, root_kp_.pub), ChainIntegrityError);
}

TEST_F(ValidatorTest, ValidateChildWrongParentHash) {
    Node bad = child1_;
    bad.parent_hash.bytes[0] ^= 0xFF; // corrupt hash
    EXPECT_THROW(validator_->validate_node(bad, root_kp_.pub), ChainIntegrityError);
}

TEST_F(ValidatorTest, ValidateChildParentMissing) {
    KeyPair uid2 = Crypto::generate_keypair();
    // uid2 has no nodes in storage
    EXPECT_THROW(validator_->validate_node(child1_, uid2.pub), NodeNotFoundError);
}

// ── validate_path ─────────────────────────────────────────────────────────────

TEST_F(ValidatorTest, ValidatePathOk) {
    // Path to node 3 is: 0 → 1 → 3
    EXPECT_NO_THROW(validator_->validate_path(root_kp_.pub, 3));
}

TEST_F(ValidatorTest, ValidatePathMissingNode) {
    // Path to node 5 requires nodes 0 → 2 → 5; node 2 is not stored.
    EXPECT_THROW(validator_->validate_path(root_kp_.pub, 5), NodeNotFoundError);
}

// ── validate_block ────────────────────────────────────────────────────────────

TEST_F(ValidatorTest, ValidateBlockOk) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 2'000'000LL,
                                 BlockType::DATA, {0x01}, child3_kp_);
    EXPECT_NO_THROW(validator_->validate_block(b, ph, child3_kp_.pub));
}

TEST_F(ValidatorTest, ValidateBlockWrongPrevHash) {
    Hash ph   = Crypto::hash_node(child3_);
    Hash fake = ph;
    fake.bytes[0] ^= 0xFF;
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 2'000'000LL,
                                 BlockType::DATA, {0x01}, child3_kp_);
    EXPECT_THROW(validator_->validate_block(b, fake, child3_kp_.pub), ChainIntegrityError);
}

TEST_F(ValidatorTest, ValidateBlockBadSignature) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 2'000'000LL,
                                 BlockType::DATA, {0x01}, child3_kp_);
    b.signature.bytes[0] ^= 0xFF;
    EXPECT_THROW(validator_->validate_block(b, ph, child3_kp_.pub), SignatureError);
}

TEST_F(ValidatorTest, ValidateBlockWrongKey) {
    Hash ph   = Crypto::hash_node(child3_);
    Block b   = make_signed_block({root_kp_.pub, 3, 0}, ph, 2'000'000LL,
                                   BlockType::DATA, {0x01}, child3_kp_);
    EXPECT_THROW(validator_->validate_block(b, ph, child1_kp_.pub), SignatureError);
}

// ── validate_branch ───────────────────────────────────────────────────────────

TEST_F(ValidatorTest, ValidateBranchEmpty) {
    // No blocks stored for node 3 → valid
    EXPECT_NO_THROW(validator_->validate_branch(root_kp_.pub, 3));
}

TEST_F(ValidatorTest, ValidateBranchSingleBlock) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 2'000'000LL,
                                 BlockType::DATA, {0xAA}, child3_kp_);
    storage_->put_block(b);
    EXPECT_NO_THROW(validator_->validate_branch(root_kp_.pub, 3));
}

TEST_F(ValidatorTest, ValidateBranchMultipleBlocks) {
    Hash ph0 = Crypto::hash_node(child3_);
    Block b0 = make_signed_block({root_kp_.pub, 3, 0}, ph0, 1'000LL,
                                  BlockType::DATA, {0x01}, child3_kp_);
    storage_->put_block(b0);

    Hash ph1 = Crypto::hash_block(b0);
    Block b1 = make_signed_block({root_kp_.pub, 3, 1}, ph1, 2'000LL,
                                  BlockType::DATA, {0x02}, child3_kp_);
    storage_->put_block(b1);

    EXPECT_NO_THROW(validator_->validate_branch(root_kp_.pub, 3));
}

TEST_F(ValidatorTest, ValidateBranchBadBlockSig) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 1'000LL,
                                 BlockType::DATA, {0x01}, child3_kp_);
    b.signature.bytes[0] ^= 0xFF;
    storage_->put_block(b);
    EXPECT_THROW(validator_->validate_branch(root_kp_.pub, 3), SignatureError);
}

TEST_F(ValidatorTest, ValidateBranchTimestampViolation) {
    Hash ph0 = Crypto::hash_node(child3_);
    Block b0 = make_signed_block({root_kp_.pub, 3, 0}, ph0, 5'000LL,
                                  BlockType::DATA, {0x01}, child3_kp_);
    storage_->put_block(b0);

    Hash ph1 = Crypto::hash_block(b0);
    Block b1 = make_signed_block({root_kp_.pub, 3, 1}, ph1, 3'000LL, // earlier!
                                  BlockType::DATA, {0x02}, child3_kp_);
    storage_->put_block(b1);

    EXPECT_THROW(validator_->validate_branch(root_kp_.pub, 3), TimestampError);
}

TEST_F(ValidatorTest, ValidateBranchKeyRotation) {
    KeyPair new_kp = Crypto::generate_keypair();

    Hash ph0 = Crypto::hash_node(child3_);
    // KEY_ROTATION block: payload = new pubkey (32 bytes), signed by OLD key.
    std::vector<uint8_t> rot_payload(new_kp.pub.bytes.begin(), new_kp.pub.bytes.end());
    Block b0 = make_signed_block({root_kp_.pub, 3, 0}, ph0, 1'000LL,
                                  BlockType::KEY_ROTATION, rot_payload, child3_kp_);
    storage_->put_block(b0);

    // Subsequent block must be signed by the NEW key.
    Hash ph1 = Crypto::hash_block(b0);
    Block b1 = make_signed_block({root_kp_.pub, 3, 1}, ph1, 2'000LL,
                                  BlockType::DATA, {0x55}, new_kp);
    storage_->put_block(b1);

    EXPECT_NO_THROW(validator_->validate_branch(root_kp_.pub, 3));
}

TEST_F(ValidatorTest, ValidateBranchKeyRotationWrongSubsequentKey) {
    KeyPair new_kp = Crypto::generate_keypair();

    Hash ph0 = Crypto::hash_node(child3_);
    std::vector<uint8_t> rot_payload(new_kp.pub.bytes.begin(), new_kp.pub.bytes.end());
    Block b0 = make_signed_block({root_kp_.pub, 3, 0}, ph0, 1'000LL,
                                  BlockType::KEY_ROTATION, rot_payload, child3_kp_);
    storage_->put_block(b0);

    // Signed by OLD key after rotation → should fail.
    Hash ph1 = Crypto::hash_block(b0);
    Block b1 = make_signed_block({root_kp_.pub, 3, 1}, ph1, 2'000LL,
                                  BlockType::DATA, {0x55}, child3_kp_); // wrong key
    storage_->put_block(b1);

    EXPECT_THROW(validator_->validate_branch(root_kp_.pub, 3), SignatureError);
}

// ── validate_seal ─────────────────────────────────────────────────────────────

TEST_F(ValidatorTest, ValidateSealOk) {
    Hash bh = Crypto::hash_node(child3_); // any hash
    KeyPair signer = Crypto::generate_keypair();
    Seal s{};
    s.signer_id  = signer.pub;
    s.block_hash = bh;
    s.mode       = SealMode::BLIND;
    s.sealed_at  = 0;
    s.signature  = Crypto::sign(bh.bytes.data(), bh.bytes.size(), signer.sec);
    EXPECT_NO_THROW(validator_->validate_seal(s));
}

TEST_F(ValidatorTest, ValidateSealBadSig) {
    Hash bh = Crypto::hash_node(root_);
    KeyPair signer = Crypto::generate_keypair();
    Seal s{};
    s.signer_id  = signer.pub;
    s.block_hash = bh;
    s.mode       = SealMode::OPEN;
    s.sealed_at  = 0;
    s.signature  = Crypto::sign(bh.bytes.data(), bh.bytes.size(), signer.sec);
    s.signature.bytes[0] ^= 0xFF;
    EXPECT_THROW(validator_->validate_seal(s), SignatureError);
}

// ── validate_co_signature ─────────────────────────────────────────────────────

TEST_F(ValidatorTest, ValidateCoSigOk) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 1'000LL,
                                 BlockType::MERGE, {0xBB}, child3_kp_);
    KeyPair partner = Crypto::generate_keypair();
    Hash bh = Crypto::hash_block(b);
    b.co_signature = Crypto::sign(bh.bytes.data(), bh.bytes.size(), partner.sec);
    EXPECT_NO_THROW(validator_->validate_co_signature(b, partner.pub));
}

TEST_F(ValidatorTest, ValidateCoSigWrongType) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 1'000LL,
                                 BlockType::DATA, {}, child3_kp_);
    EXPECT_THROW(validator_->validate_co_signature(b, child3_kp_.pub),
                 InvalidArgumentError);
}

TEST_F(ValidatorTest, ValidateCoSigMissing) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 1'000LL,
                                 BlockType::MERGE, {}, child3_kp_);
    // co_signature not set
    EXPECT_THROW(validator_->validate_co_signature(b, child3_kp_.pub), SignatureError);
}

TEST_F(ValidatorTest, ValidateCoSigBadSig) {
    Hash ph = Crypto::hash_node(child3_);
    Block b = make_signed_block({root_kp_.pub, 3, 0}, ph, 1'000LL,
                                 BlockType::MERGE, {}, child3_kp_);
    KeyPair partner = Crypto::generate_keypair();
    Hash bh = Crypto::hash_block(b);
    Signature co = Crypto::sign(bh.bytes.data(), bh.bytes.size(), partner.sec);
    co.bytes[0] ^= 0xFF;
    b.co_signature = co;
    EXPECT_THROW(validator_->validate_co_signature(b, partner.pub), SignatureError);
}
