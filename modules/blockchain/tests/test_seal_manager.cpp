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

TEST_F(SealManagerTest, ComputeWitnessedTimeNoWitnessesReturnsNullopt) {
    // Empty external_blocks table → no witnesses.
    BlockAddress addr{};
    EXPECT_FALSE(sealmgr_->compute_witnessed_time(addr).has_value());
}

TEST_F(SealManagerTest, ComputeWitnessedTimeFindsEarliestExternalMergeBlock) {
    // Build "our" identity and a DATA block in our branch.
    const KeyPair own_kp   = Crypto::generate_keypair();
    const UserId  own_uid  = own_kp.pub;
    constexpr NodeIndex leaf = 0x7FFF'FFFFu;

    Block own_block{};
    own_block.address           = {own_uid, leaf, 0};
    own_block.prev_hash         = Hash::zero();
    own_block.timestamp_claimed = 1'000'000LL;
    own_block.type              = BlockType::DATA;
    own_block.payload           = {0xAB};
    {
        auto b = Serializer::encode(own_block);
        own_block.signature = Crypto::sign(b.data(), b.size(), own_kp.sec);
    }
    const Hash own_hash = Crypto::hash_block(own_block);

    // Build a partner's MERGE block that references our block.
    const KeyPair partner_kp  = Crypto::generate_keypair();
    constexpr NodeIndex plf   = 0x7FFF'FFFEu;

    MergePayload mp{};
    mp.partner_last_address = {own_uid, leaf, 0};
    mp.partner_last_hash    = own_hash;
    mp.merge_timestamp      = 2'000'000LL;

    Block merge_block{};
    merge_block.address           = {partner_kp.pub, plf, 0};
    merge_block.prev_hash         = Hash::zero();
    merge_block.timestamp_claimed = 2'000'000LL;
    merge_block.type              = BlockType::MERGE;
    merge_block.payload           = Serializer::encode(mp);
    {
        auto b = Serializer::encode(merge_block);
        merge_block.signature = Crypto::sign(b.data(), b.size(), partner_kp.sec);
    }

    // A second, later witness.
    const KeyPair partner2_kp = Crypto::generate_keypair();
    Block merge_block2 = merge_block;
    merge_block2.address           = {partner2_kp.pub, plf, 0};
    merge_block2.timestamp_claimed = 3'000'000LL;
    mp.merge_timestamp             = 3'000'000LL;
    merge_block2.payload           = Serializer::encode(mp);
    {
        auto b = Serializer::encode(merge_block2);
        merge_block2.signature = Crypto::sign(b.data(), b.size(), partner2_kp.sec);
    }

    storage_->put_external_block(merge_block);
    storage_->put_external_block(merge_block2);

    // The earliest witness timestamp should be returned.
    auto wt = sealmgr_->compute_witnessed_time({own_uid, leaf, 0});
    ASSERT_TRUE(wt.has_value());
    EXPECT_EQ(*wt, 2'000'000LL);
}

TEST_F(SealManagerTest, ComputeWitnessedTimeIgnoresUnrelatedMergeBlocks) {
    // A MERGE block referencing a different user should not count.
    const KeyPair other_kp  = Crypto::generate_keypair();
    const KeyPair signer_kp = Crypto::generate_keypair();
    constexpr NodeIndex leaf = 0x7FFF'FFFFu;

    MergePayload mp{};
    mp.partner_last_address = {other_kp.pub, leaf, 0};  // different user
    mp.partner_last_hash    = Hash::zero();
    mp.merge_timestamp      = 5'000'000LL;

    Block merge_block{};
    merge_block.address           = {signer_kp.pub, leaf, 0};
    merge_block.prev_hash         = Hash::zero();
    merge_block.timestamp_claimed = 5'000'000LL;
    merge_block.type              = BlockType::MERGE;
    merge_block.payload           = Serializer::encode(mp);
    {
        auto b = Serializer::encode(merge_block);
        merge_block.signature = Crypto::sign(b.data(), b.size(), signer_kp.sec);
    }
    storage_->put_external_block(merge_block);

    // Query for a completely different user → no witnesses.
    const KeyPair target_kp = Crypto::generate_keypair();
    EXPECT_FALSE(sealmgr_->compute_witnessed_time({target_kp.pub, leaf, 0}).has_value());
}
