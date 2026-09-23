#include "aggregator/aggregator.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace aggregator;
using namespace blockchain;

// ── Helpers ───────────────────────────────────────────────────────────────────

static Block make_block(const UserId& uid, NodeIndex ni, BlockIndex bi,
                         const std::vector<uint8_t>& payload) {
    Block b{};
    b.address           = {uid, ni, bi};
    b.prev_hash         = Hash::zero();
    b.timestamp_claimed = static_cast<Timestamp>(bi) * 1000LL;
    b.type              = BlockType::DATA;
    b.payload           = payload;
    b.signature         = Signature::null();
    auto bytes          = Serializer::encode(b);
    KeyPair kp          = Crypto::generate_keypair();
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

static UserId make_uid(uint8_t fill) {
    UserId u; u.bytes.fill(fill); return u;
}

// ── Fixture ───────────────────────────────────────────────────────────────────

class AggregatorStorageTest : public ::testing::Test {
protected:
    std::filesystem::path       db_path_;
    std::unique_ptr<AggregatorStorage> storage_;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_agg_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }
};

// ── add_block ─────────────────────────────────────────────────────────────────

TEST_F(AggregatorStorageTest, AddBlockNew) {
    Block b = make_block(make_uid(0x01), 0, 0, {0xAA, 0xBB});
    EXPECT_TRUE(storage_->add_block(b));
    EXPECT_EQ(storage_->block_count(), 1u);
}

TEST_F(AggregatorStorageTest, AddBlockDuplicateReturnsFalse) {
    Block b = make_block(make_uid(0x01), 0, 0, {0xCC});
    EXPECT_TRUE(storage_->add_block(b));
    EXPECT_FALSE(storage_->add_block(b)); // same block → duplicate
    EXPECT_EQ(storage_->block_count(), 1u);
}

TEST_F(AggregatorStorageTest, AddMultipleBlocks) {
    UserId uid = make_uid(0x01);
    storage_->add_block(make_block(uid, 0, 0, {0x01}));
    storage_->add_block(make_block(uid, 0, 1, {0x02}));
    storage_->add_block(make_block(uid, 0, 2, {0x03}));
    EXPECT_EQ(storage_->block_count(), 3u);
}

// ── has_block / get_block ─────────────────────────────────────────────────────

TEST_F(AggregatorStorageTest, HasBlockTrue) {
    Block b = make_block(make_uid(0x01), 5, 0, {0xDE});
    storage_->add_block(b);
    EXPECT_TRUE(storage_->has_block(b.address));
}

TEST_F(AggregatorStorageTest, HasBlockFalse) {
    BlockAddress addr{make_uid(0x01), 5, 0};
    EXPECT_FALSE(storage_->has_block(addr));
}

TEST_F(AggregatorStorageTest, GetBlockFound) {
    Block b = make_block(make_uid(0x02), 3, 7, {0x11, 0x22, 0x33});
    storage_->add_block(b);
    auto got = storage_->get_block(b.address);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->address, b.address);
    EXPECT_EQ(got->payload, b.payload);
}

TEST_F(AggregatorStorageTest, GetBlockNotFound) {
    EXPECT_FALSE(storage_->get_block({make_uid(0xFF), 0, 0}).has_value());
}

// ── has_block_hash / get_block_by_hash ───────────────────────────────────────

TEST_F(AggregatorStorageTest, HasBlockHashTrue) {
    Block b = make_block(make_uid(0x01), 0, 0, {0xAB});
    storage_->add_block(b);
    Hash bh = Crypto::hash_block(b);
    EXPECT_TRUE(storage_->has_block_hash(bh));
}

TEST_F(AggregatorStorageTest, HasBlockHashFalse) {
    Hash h; h.bytes.fill(0x42);
    EXPECT_FALSE(storage_->has_block_hash(h));
}

TEST_F(AggregatorStorageTest, GetBlockByHash) {
    Block b = make_block(make_uid(0x01), 0, 0, {0xDE, 0xAD});
    storage_->add_block(b);
    Hash bh = Crypto::hash_block(b);
    auto got = storage_->get_block_by_hash(bh);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(got->payload, b.payload);
}

// ── Ideas (dedup by payload hash) ─────────────────────────────────────────────

TEST_F(AggregatorStorageTest, SamePayloadCountedOnce) {
    std::vector<uint8_t> payload = {0xCA, 0xFE};

    // Two different users, same payload.
    Block b1 = make_block(make_uid(0x01), 0, 0, payload);
    Block b2 = make_block(make_uid(0x02), 0, 0, payload);
    storage_->add_block(b1);
    storage_->add_block(b2);

    EXPECT_EQ(storage_->idea_count(), 1u); // same payload_hash → one idea
    EXPECT_EQ(storage_->block_count(), 2u);

    Hash ph = Crypto::hash(payload.data(), payload.size());
    auto idea = storage_->get_idea(ph);
    ASSERT_TRUE(idea.has_value());
    EXPECT_EQ(idea->witnesses.size(), 2u);
    EXPECT_EQ(idea->payload, payload);
}

TEST_F(AggregatorStorageTest, DifferentPayloadsDifferentIdeas) {
    storage_->add_block(make_block(make_uid(0x01), 0, 0, {0x01}));
    storage_->add_block(make_block(make_uid(0x01), 0, 1, {0x02}));
    storage_->add_block(make_block(make_uid(0x01), 0, 2, {0x03}));
    EXPECT_EQ(storage_->idea_count(), 3u);
}

TEST_F(AggregatorStorageTest, AllIdeasReturnAll) {
    storage_->add_block(make_block(make_uid(0x01), 0, 0, {0xAA}));
    storage_->add_block(make_block(make_uid(0x02), 0, 0, {0xBB}));
    storage_->add_block(make_block(make_uid(0x03), 0, 0, {0xAA})); // same as first
    auto ideas = storage_->all_ideas();
    EXPECT_EQ(ideas.size(), 2u);
}

TEST_F(AggregatorStorageTest, GetIdeaNotFound) {
    Hash ph; ph.bytes.fill(0x99);
    EXPECT_FALSE(storage_->get_idea(ph).has_value());
}

// ── all_block_hashes ──────────────────────────────────────────────────────────

TEST_F(AggregatorStorageTest, AllBlockHashesCount) {
    storage_->add_block(make_block(make_uid(0x01), 0, 0, {0x01}));
    storage_->add_block(make_block(make_uid(0x01), 0, 1, {0x02}));
    storage_->add_block(make_block(make_uid(0x01), 0, 2, {0x03}));
    EXPECT_EQ(storage_->all_block_hashes().size(), 3u);
}

TEST_F(AggregatorStorageTest, AllBlockHashesEmpty) {
    EXPECT_TRUE(storage_->all_block_hashes().empty());
}

// ── Persistence across reopen ─────────────────────────────────────────────────

TEST_F(AggregatorStorageTest, PersistenceAcrossReopen) {
    std::vector<uint8_t> payload = {0xFF, 0x00, 0xAB};
    Block b = make_block(make_uid(0x05), 0, 0, payload);
    storage_->add_block(b);

    // Reopen.
    storage_.reset();
    storage_ = std::make_unique<AggregatorStorage>(db_path_);

    EXPECT_EQ(storage_->block_count(), 1u);
    EXPECT_EQ(storage_->idea_count(),  1u);
    EXPECT_TRUE(storage_->has_block(b.address));
    Hash ph = Crypto::hash(payload.data(), payload.size());
    auto idea = storage_->get_idea(ph);
    ASSERT_TRUE(idea.has_value());
    EXPECT_EQ(idea->payload, payload);
}

// ── Склад путей ключей (sync.md §7.5) ─────────────────────────────────────────
//
// Склад возит байты и ничего не проверяет: что путь ведёт к этому ключу,
// решает читатель (blockchain.md §7.5). Отсюда и требования — отдать всё, что
// положили под ключ, не смешивая ключи, и не плодить копии одного и того же.

TEST_F(AggregatorStorageTest, KeyPathWarehouseKeepsClaimsPerKey) {
    const PublicKey pel = make_uid(0xE1), boris = make_uid(0xB0);
    const std::vector<uint8_t> path_a{1, 2, 3}, path_b{4, 5, 6}, other{7};

    EXPECT_TRUE(storage_->put_keypath_bytes(pel, path_a));
    EXPECT_TRUE(storage_->put_keypath_bytes(pel, path_b));
    EXPECT_TRUE(storage_->put_keypath_bytes(boris, other));

    EXPECT_EQ(storage_->get_keypath_bytes(pel).size(), 2u);
    ASSERT_EQ(storage_->get_keypath_bytes(boris).size(), 1u);
    EXPECT_EQ(storage_->get_keypath_bytes(boris)[0], other);
}

TEST_F(AggregatorStorageTest, KeyPathWarehouseDedupesIdenticalBytes) {
    const PublicKey pel = make_uid(0xE2);
    const std::vector<uint8_t> path{9, 9, 9};

    EXPECT_TRUE(storage_->put_keypath_bytes(pel, path));
    EXPECT_FALSE(storage_->put_keypath_bytes(pel, path));   // тот же путь — та же запись
    EXPECT_EQ(storage_->get_keypath_bytes(pel).size(), 1u);
}

TEST_F(AggregatorStorageTest, KeyPathManifestListsKeysOnce) {
    const PublicKey a = make_uid(0x10), b = make_uid(0x20);
    storage_->put_keypath_bytes(a, {1});
    storage_->put_keypath_bytes(a, {2});
    storage_->put_keypath_bytes(b, {3});

    const auto keys = storage_->all_keypath_keys();
    EXPECT_EQ(keys.size(), 2u);
}

TEST_F(AggregatorStorageTest, KeyPathWarehouseEmptyForUnknownKey) {
    EXPECT_TRUE(storage_->get_keypath_bytes(make_uid(0x33)).empty());
    EXPECT_TRUE(storage_->all_keypath_keys().empty());
}
