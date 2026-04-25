#include "blockchain/storage.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>
#include <filesystem>

using namespace blockchain;

// ── Test fixture ──────────────────────────────────────────────────────────────

class StorageTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<LmdbStorage> storage_;

    void SetUp() override {
        static int counter = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_storage_test_" + std::to_string(++counter));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<LmdbStorage>(db_path_);
    }

    void TearDown() override {
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    static UserId make_uid(uint8_t fill) {
        UserId u; u.bytes.fill(fill); return u;
    }
    static Hash make_hash(uint8_t fill) {
        Hash h; h.bytes.fill(fill); return h;
    }
    static Signature make_sig(uint8_t fill) {
        Signature s; s.bytes.fill(fill); return s;
    }

    static Node make_node(NodeIndex idx, uint8_t fill = 0xAB) {
        Node n{};
        n.index             = idx;
        n.structural_pubkey = make_uid(fill);
        n.working_pubkey    = make_uid(static_cast<uint8_t>(fill + 1));
        n.parent_hash       = make_hash(fill);
        n.parent_sig        = make_sig(fill);
        n.created_at        = 1'700'000'000LL + idx;
        return n;
    }

    static Block make_block(const UserId& uid, NodeIndex ni, BlockIndex bi,
                            uint8_t payload_fill = 0x42) {
        Block b{};
        b.address.user_id    = uid;
        b.address.node_index  = ni;
        b.address.block_index = bi;
        b.prev_hash           = make_hash(static_cast<uint8_t>(bi == 0 ? 0x00 : bi - 1));
        b.timestamp_claimed   = 1'700'000'000LL + bi;
        b.type                = BlockType::DATA;
        b.payload             = {payload_fill};
        b.signature           = make_sig(payload_fill);
        return b;
    }

    static Seal make_seal(const Hash& bh, uint8_t fill) {
        Seal s{};
        s.signer_id  = make_uid(fill);
        s.block_hash = bh;
        s.signature  = make_sig(fill);
        s.mode       = SealMode::BLIND;
        s.sealed_at  = 1'700'000'042LL;
        return s;
    }
};

// ── Node CRUD ─────────────────────────────────────────────────────────────────

TEST_F(StorageTest, PutAndGetNode) {
    auto uid = make_uid(0x01);
    Node n   = make_node(0);
    storage_->put_node(uid, n);

    Node got = storage_->get_node(uid, 0);
    EXPECT_EQ(got, n);
}

TEST_F(StorageTest, HasNodeTrue) {
    auto uid = make_uid(0x01);
    storage_->put_node(uid, make_node(0));
    EXPECT_TRUE(storage_->has_node(uid, 0));
}

TEST_F(StorageTest, HasNodeFalse) {
    auto uid = make_uid(0x01);
    EXPECT_FALSE(storage_->has_node(uid, 99));
}

TEST_F(StorageTest, GetNodeNotFound) {
    auto uid = make_uid(0x01);
    EXPECT_THROW(storage_->get_node(uid, 0), NodeNotFoundError);
    try { storage_->get_node(uid, 0); }
    catch (const NodeNotFoundError& e) { EXPECT_EQ(e.missing_index(), 0u); }
}

TEST_F(StorageTest, PutNodeDuplicateThrows) {
    auto uid = make_uid(0x01);
    storage_->put_node(uid, make_node(0));
    EXPECT_THROW(storage_->put_node(uid, make_node(0)), InvalidArgumentError);
}

TEST_F(StorageTest, MultipleNodesDifferentIndex) {
    auto uid = make_uid(0x01);
    storage_->put_node(uid, make_node(0));
    storage_->put_node(uid, make_node(1));
    storage_->put_node(uid, make_node(2));

    EXPECT_EQ(storage_->get_node(uid, 0).index, 0u);
    EXPECT_EQ(storage_->get_node(uid, 1).index, 1u);
    EXPECT_EQ(storage_->get_node(uid, 2).index, 2u);
}

TEST_F(StorageTest, NodeIsolatedByUser) {
    auto uid1 = make_uid(0x01);
    auto uid2 = make_uid(0x02);
    storage_->put_node(uid1, make_node(0));

    // uid2 doesn't have node 0
    EXPECT_FALSE(storage_->has_node(uid2, 0));
    EXPECT_THROW(storage_->get_node(uid2, 0), NodeNotFoundError);
}

// ── Block CRUD ────────────────────────────────────────────────────────────────

TEST_F(StorageTest, PutAndGetBlock) {
    auto uid = make_uid(0x01);
    Block b  = make_block(uid, 5, 0);
    storage_->put_block(b);

    Block got = storage_->get_block(b.address);
    EXPECT_EQ(got.address,    b.address);
    EXPECT_EQ(got.prev_hash,  b.prev_hash);
    EXPECT_EQ(got.type,       b.type);
    EXPECT_EQ(got.payload,    b.payload);
    EXPECT_EQ(got.signature,  b.signature);
}

TEST_F(StorageTest, HasBlockTrue) {
    auto uid = make_uid(0x01);
    Block b  = make_block(uid, 5, 0);
    storage_->put_block(b);
    EXPECT_TRUE(storage_->has_block(b.address));
}

TEST_F(StorageTest, HasBlockFalse) {
    BlockAddress addr{make_uid(0x01), 5, 0};
    EXPECT_FALSE(storage_->has_block(addr));
}

TEST_F(StorageTest, GetBlockNotFound) {
    BlockAddress addr{make_uid(0x01), 5, 0};
    EXPECT_THROW(storage_->get_block(addr), BlockNotFoundError);
}

TEST_F(StorageTest, PutBlockDuplicateThrows) {
    auto uid = make_uid(0x01);
    Block b  = make_block(uid, 5, 0);
    storage_->put_block(b);
    EXPECT_THROW(storage_->put_block(b), InvalidArgumentError);
}

// ── branch_tip_index ──────────────────────────────────────────────────────────

TEST_F(StorageTest, BranchTipEmptyBranch) {
    auto uid = make_uid(0x01);
    EXPECT_EQ(storage_->branch_tip_index(uid, 5), std::nullopt);
}

TEST_F(StorageTest, BranchTipSingleBlock) {
    auto uid = make_uid(0x01);
    storage_->put_block(make_block(uid, 5, 0));
    EXPECT_EQ(storage_->branch_tip_index(uid, 5), std::optional<BlockIndex>(0));
}

TEST_F(StorageTest, BranchTipMultipleBlocks) {
    auto uid = make_uid(0x01);
    for (BlockIndex i = 0; i <= 4; ++i)
        storage_->put_block(make_block(uid, 5, i));
    EXPECT_EQ(storage_->branch_tip_index(uid, 5), std::optional<BlockIndex>(4));
}

TEST_F(StorageTest, BranchTipIsolatedByNode) {
    auto uid = make_uid(0x01);
    storage_->put_block(make_block(uid, 3, 0));
    storage_->put_block(make_block(uid, 3, 1));
    storage_->put_block(make_block(uid, 7, 0));

    EXPECT_EQ(storage_->branch_tip_index(uid, 3), std::optional<BlockIndex>(1));
    EXPECT_EQ(storage_->branch_tip_index(uid, 7), std::optional<BlockIndex>(0));
    EXPECT_EQ(storage_->branch_tip_index(uid, 9), std::nullopt);
}

TEST_F(StorageTest, BranchTipIsolatedByUser) {
    auto uid1 = make_uid(0x01);
    auto uid2 = make_uid(0x02);
    storage_->put_block(make_block(uid1, 5, 0));
    storage_->put_block(make_block(uid1, 5, 1));
    storage_->put_block(make_block(uid2, 5, 0));

    EXPECT_EQ(storage_->branch_tip_index(uid1, 5), std::optional<BlockIndex>(1));
    EXPECT_EQ(storage_->branch_tip_index(uid2, 5), std::optional<BlockIndex>(0));
}

// ── Seals ─────────────────────────────────────────────────────────────────────

TEST_F(StorageTest, GetSealsEmpty) {
    EXPECT_TRUE(storage_->get_seals(make_hash(0x01)).empty());
}

TEST_F(StorageTest, PutAndGetSingleSeal) {
    auto bh   = make_hash(0xBB);
    Seal seal = make_seal(bh, 0xAA);
    storage_->put_seal(seal);

    auto seals = storage_->get_seals(bh);
    ASSERT_EQ(seals.size(), 1u);
    EXPECT_EQ(seals[0], seal);
}

TEST_F(StorageTest, PutMultipleSeals) {
    auto bh = make_hash(0xBB);
    storage_->put_seal(make_seal(bh, 0x01));
    storage_->put_seal(make_seal(bh, 0x02));
    storage_->put_seal(make_seal(bh, 0x03));

    auto seals = storage_->get_seals(bh);
    ASSERT_EQ(seals.size(), 3u);
}

TEST_F(StorageTest, SealsIsolatedByHash) {
    auto bh1 = make_hash(0x11);
    auto bh2 = make_hash(0x22);
    storage_->put_seal(make_seal(bh1, 0xAA));
    storage_->put_seal(make_seal(bh2, 0xBB));

    EXPECT_EQ(storage_->get_seals(bh1).size(), 1u);
    EXPECT_EQ(storage_->get_seals(bh2).size(), 1u);
}

// ── External blocks ───────────────────────────────────────────────────────────

TEST_F(StorageTest, PutAndGetExternalBlock) {
    auto uid = make_uid(0x02); // different user
    Block b  = make_block(uid, 3, 0);
    storage_->put_external_block(b);

    EXPECT_TRUE(storage_->has_external_block(b.address));
    Block got = storage_->get_external_block(b.address);
    EXPECT_EQ(got.address, b.address);
}

TEST_F(StorageTest, ExternalBlockNotFound) {
    BlockAddress addr{make_uid(0x02), 3, 0};
    EXPECT_THROW(storage_->get_external_block(addr), BlockNotFoundError);
}

TEST_F(StorageTest, ExternalBlockDuplicateThrows) {
    auto uid = make_uid(0x02);
    Block b  = make_block(uid, 3, 0);
    storage_->put_external_block(b);
    EXPECT_THROW(storage_->put_external_block(b), InvalidArgumentError);
}

// ── Persistence ───────────────────────────────────────────────────────────────

TEST_F(StorageTest, PersistenceAcrossReopen) {
    auto uid = make_uid(0x01);
    storage_->put_node(uid, make_node(0));
    storage_->put_block(make_block(uid, 5, 0));

    // Close and reopen
    storage_.reset();
    storage_ = std::make_unique<LmdbStorage>(db_path_);

    EXPECT_TRUE(storage_->has_node(uid, 0));
    EXPECT_EQ(storage_->get_node(uid, 0).index, 0u);
    EXPECT_TRUE(storage_->has_block(BlockAddress{uid, 5, 0}));
}
