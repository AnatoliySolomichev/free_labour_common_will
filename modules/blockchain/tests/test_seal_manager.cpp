#include "blockchain/seal_manager.h"
#include "blockchain/validator.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace blockchain;

// ── Fixture ───────────────────────────────────────────────────────────────────

class SealManagerTest : public ::testing::Test {
protected:
    std::filesystem::path   db_path_;
    std::unique_ptr<LmdbStorage>  storage_;
    std::unique_ptr<Validator>    validator_;
    std::unique_ptr<SealManager>  sealmgr_;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_seal_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_   = std::make_unique<LmdbStorage>(db_path_);
        validator_ = std::make_unique<Validator>(*storage_);
        sealmgr_   = std::make_unique<SealManager>(*storage_);
    }

    void TearDown() override {
        sealmgr_.reset(); validator_.reset(); storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    static Block make_dummy_block() {
        KeyPair kp = Crypto::generate_keypair();
        Block b{};
        b.address           = {kp.pub, 0x7FFF'FFFFu, 0};
        b.prev_hash         = Hash::zero();
        b.timestamp_claimed = 1'000'000LL;
        b.type              = BlockType::DATA;
        b.payload           = {0xDE, 0xAD};
        b.signature         = Signature::null();
        auto bytes          = Serializer::encode(b);
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        return b;
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(SealManagerTest, CreateBlindSealStoredAndRetrievable) {
    KeyPair signer = Crypto::generate_keypair();
    Hash bh;
    bh.bytes.fill(0x42);

    Seal s = sealmgr_->create_seal(bh, signer, SealMode::BLIND);
    EXPECT_EQ(s.signer_id,  signer.pub);
    EXPECT_EQ(s.block_hash, bh);
    EXPECT_EQ(s.mode,       SealMode::BLIND);

    auto seals = sealmgr_->get_seals(bh);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0], s);
}

TEST_F(SealManagerTest, CreateOpenSealStoredAndRetrievable) {
    Block b  = make_dummy_block();
    Hash  bh = Crypto::hash_block(b);
    KeyPair signer = Crypto::generate_keypair();

    Seal s = sealmgr_->create_open_seal(b, signer);
    EXPECT_EQ(s.block_hash, bh);
    EXPECT_EQ(s.mode,       SealMode::OPEN);

    auto seals = sealmgr_->get_seals(bh);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0], s);
}

TEST_F(SealManagerTest, BlindSealSignatureValid) {
    KeyPair signer = Crypto::generate_keypair();
    Hash bh;
    bh.bytes.fill(0x77);

    Seal s = sealmgr_->create_seal(bh, signer, SealMode::BLIND);
    EXPECT_NO_THROW(validator_->validate_seal(s));
}

TEST_F(SealManagerTest, OpenSealSignatureValid) {
    Block b  = make_dummy_block();
    KeyPair signer = Crypto::generate_keypair();

    Seal s = sealmgr_->create_open_seal(b, signer);
    EXPECT_NO_THROW(validator_->validate_seal(s));
}

TEST_F(SealManagerTest, MultipleSignersOnSameBlock) {
    Hash bh;
    bh.bytes.fill(0x11);

    KeyPair s1 = Crypto::generate_keypair();
    KeyPair s2 = Crypto::generate_keypair();
    KeyPair s3 = Crypto::generate_keypair();

    sealmgr_->create_seal(bh, s1, SealMode::BLIND);
    sealmgr_->create_seal(bh, s2, SealMode::OPEN);
    sealmgr_->create_seal(bh, s3, SealMode::BLIND);

    auto seals = sealmgr_->get_seals(bh);
    EXPECT_EQ(seals.size(), 3u);
}

TEST_F(SealManagerTest, GetSealsEmptyHash) {
    Hash bh;
    bh.bytes.fill(0xFF);
    EXPECT_TRUE(sealmgr_->get_seals(bh).empty());
}

TEST_F(SealManagerTest, ComputeWitnessedTimeMvpReturnsNullopt) {
    BlockAddress addr{};
    EXPECT_FALSE(sealmgr_->compute_witnessed_time(addr).has_value());
}
