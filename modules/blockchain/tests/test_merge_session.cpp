#include "blockchain/merge_session.h"
#include "blockchain/blockchain.h"
#include "blockchain/validator.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <map>

using namespace blockchain;

static constexpr NodeIndex LEAF = 0x7FFF'FFFFu; // leftmost depth-31 leaf

// ── Per-user helper ───────────────────────────────────────────────────────────

struct UserCtx {
    std::filesystem::path   db_path;
    std::unique_ptr<LmdbStorage>  storage;
    std::unique_ptr<Validator>    validator;
    std::unique_ptr<Blockchain>   bc;
    std::unique_ptr<MergeSession> ms;

    KeyPair                      root_kp;
    std::map<NodeIndex, KeyPair> path_keys;

    UserCtx(const std::filesystem::path& base, int id) {
        db_path = base / ("user_" + std::to_string(id));
        std::filesystem::remove_all(db_path);
        storage   = std::make_unique<LmdbStorage>(db_path);
        validator = std::make_unique<Validator>(*storage);
        bc        = std::make_unique<Blockchain>(*storage, *validator);
        ms        = std::make_unique<MergeSession>(*storage, *validator);

        root_kp        = Crypto::generate_keypair();
        path_keys[0]   = root_kp;
        bc->create_identity(root_kp);
    }

    ~UserCtx() { ms.reset(); bc.reset(); validator.reset(); storage.reset(); }

    // Create all path nodes to LEAF and return the leaf keypair.
    KeyPair setup_leaf() {
        for (NodeIndex idx : path_indices(LEAF))
            if (path_keys.find(idx) == path_keys.end())
                path_keys[idx] = Crypto::generate_keypair();
        auto kf = [&](NodeIndex i) { return path_keys.at(i); };
        bc->ensure_path(root_kp.pub, LEAF, kf);
        return path_keys.at(LEAF);
    }
};

// ── Fixture ───────────────────────────────────────────────────────────────────

class MergeSessionTest : public ::testing::Test {
protected:
    std::filesystem::path base_dir_;
    std::unique_ptr<UserCtx> alice_;
    std::unique_ptr<UserCtx> bob_;

    void SetUp() override {
        static int cnt = 0;
        base_dir_ = std::filesystem::temp_directory_path() /
                    ("bc_merge_test_" + std::to_string(++cnt));
        std::filesystem::create_directories(base_dir_);
        alice_ = std::make_unique<UserCtx>(base_dir_, 1);
        bob_   = std::make_unique<UserCtx>(base_dir_, 2);
    }

    void TearDown() override {
        alice_.reset(); bob_.reset();
        std::filesystem::remove_all(base_dir_);
    }
};

// ── prepare_tip ───────────────────────────────────────────────────────────────

TEST_F(MergeSessionTest, PrepareTipEmptyBranch) {
    alice_->setup_leaf();
    BranchTipInfo tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    EXPECT_EQ(tip.tip_address.block_index, EMPTY_BRANCH_INDEX);
    ASSERT_FALSE(tip.path.empty());
    EXPECT_EQ(tip.path.front().index, 0u);
    EXPECT_EQ(tip.path.back().index,  LEAF);
    EXPECT_FALSE(tip.tip_block.has_value());

    // tip_hash for empty branch == hash of leaf node
    Node leaf     = alice_->storage->get_node(alice_->root_kp.pub, LEAF);
    Hash expected = Crypto::hash_node(leaf);
    EXPECT_EQ(tip.tip_hash, expected);
}

TEST_F(MergeSessionTest, PrepareTipWithBlocks) {
    KeyPair leaf_kp = alice_->setup_leaf();
    Block b0 = alice_->bc->append_data_block(
        alice_->root_kp.pub, LEAF, {0x01}, leaf_kp, 1'000LL);
    Block b1 = alice_->bc->append_data_block(
        alice_->root_kp.pub, LEAF, {0x02}, leaf_kp, 2'000LL);

    BranchTipInfo tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);
    EXPECT_EQ(tip.tip_address.block_index, 1u);
    EXPECT_EQ(tip.tip_hash, Crypto::hash_block(b1));
    ASSERT_TRUE(tip.tip_block.has_value());
    EXPECT_EQ(tip.tip_block->address, b1.address);
}

// ── verify_partner_tip ────────────────────────────────────────────────────────

TEST_F(MergeSessionTest, VerifyPartnerTipOk) {
    alice_->setup_leaf();
    BranchTipInfo tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    // Bob verifies Alice's tip
    EXPECT_NO_THROW(bob_->ms->verify_partner_tip(tip));
}

TEST_F(MergeSessionTest, VerifyPartnerTipBadNodeSig) {
    alice_->setup_leaf();
    BranchTipInfo tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    // Corrupt the root node's self-signature
    tip.path[0].parent_sig.bytes[0] ^= 0xFF;
    EXPECT_THROW(bob_->ms->verify_partner_tip(tip), SignatureError);
}

TEST_F(MergeSessionTest, VerifyPartnerTipBadParentHash) {
    alice_->setup_leaf();
    BranchTipInfo tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    // Corrupt an intermediate node's parent_hash
    tip.path[1].parent_hash.bytes[0] ^= 0xFF;
    EXPECT_THROW(bob_->ms->verify_partner_tip(tip), ChainIntegrityError);
}

TEST_F(MergeSessionTest, VerifyPartnerTipEmptyPathThrows) {
    BranchTipInfo tip{};
    EXPECT_THROW(bob_->ms->verify_partner_tip(tip), ChainIntegrityError);
}

// ── Full bilateral merge protocol (§6.4) ──────────────────────────────────────

TEST_F(MergeSessionTest, FullMergeProtocol) {
    KeyPair alice_leaf = alice_->setup_leaf();
    KeyPair bob_leaf   = bob_->setup_leaf();

    // Add some data blocks to both branches
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0xAA}, alice_leaf, 1'000LL);
    bob_->bc->append_data_block(  bob_->root_kp.pub,   LEAF, {0xBB}, bob_leaf,   1'000LL);

    // Step 1: exchange tips
    BranchTipInfo alice_tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);
    BranchTipInfo bob_tip   = bob_->ms->prepare_tip(  bob_->root_kp.pub,   LEAF);

    EXPECT_NO_THROW(alice_->ms->verify_partner_tip(bob_tip));
    EXPECT_NO_THROW(bob_->ms->verify_partner_tip(alice_tip));

    // Step 2: create pending merge blocks (exchange snapshots first)
    Timestamp ts = 2'000LL;
    MergeSnapshot alice_snap = alice_->ms->snapshot_for(alice_->root_kp.pub, LEAF);
    MergeSnapshot bob_snap   = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);

    PendingMergeBlock alice_pending = alice_->ms->create_pending(
        alice_->root_kp.pub, LEAF, bob_tip, bob_snap, alice_leaf, ts, 1u);
    PendingMergeBlock bob_pending = bob_->ms->create_pending(
        bob_->root_kp.pub, LEAF, alice_tip, alice_snap, bob_leaf, ts, 1u);

    // Both sides committed the same union snapshot (commutative merge).
    MergePayload alice_mp = Serializer::decode_merge_payload(
        alice_pending.draft.payload.data(), alice_pending.draft.payload.size());
    MergePayload bob_mp = Serializer::decode_merge_payload(
        bob_pending.draft.payload.data(), bob_pending.draft.payload.size());
    EXPECT_EQ(alice_mp.merkle_root, bob_mp.merkle_root);
    EXPECT_EQ(alice_mp.hll_hash,    bob_mp.hll_hash);

    // Drafts are persisted
    EXPECT_TRUE(alice_->storage->has_block(alice_pending.draft.address));
    EXPECT_TRUE(bob_->storage->has_block(bob_pending.draft.address));

    // Step 3: co-sign partner's draft hash
    Signature alice_co_sig = alice_->ms->co_sign(bob_pending.draft_hash, alice_leaf);
    Signature bob_co_sig   = bob_->ms->co_sign(alice_pending.draft_hash, bob_leaf);

    // Step 4: finalize
    Block alice_final = alice_->ms->finalize(alice_pending, bob_co_sig,   bob_leaf.pub);
    Block bob_final   = bob_->ms->finalize(  bob_pending,   alice_co_sig, alice_leaf.pub);

    ASSERT_TRUE(alice_final.co_signature.has_value());
    ASSERT_TRUE(bob_final.co_signature.has_value());

    // Validate co-signatures
    EXPECT_NO_THROW(alice_->validator->validate_co_signature(alice_final, bob_leaf.pub));
    EXPECT_NO_THROW(bob_->validator->validate_co_signature(  bob_final, alice_leaf.pub));

    // co_sig stored as Seal in seals table
    auto alice_seals = alice_->storage->get_seals(alice_pending.draft_hash);
    ASSERT_EQ(alice_seals.size(), 1u);
    EXPECT_EQ(alice_seals[0].signer_id, bob_leaf.pub);
    EXPECT_EQ(alice_seals[0].mode, SealMode::OPEN);

    auto bob_seals = bob_->storage->get_seals(bob_pending.draft_hash);
    ASSERT_EQ(bob_seals.size(), 1u);
    EXPECT_EQ(bob_seals[0].signer_id, alice_leaf.pub);
}

// ── import_partner_data ───────────────────────────────────────────────────────

TEST_F(MergeSessionTest, ImportPartnerDataEmptyBranch) {
    alice_->setup_leaf();
    bob_->setup_leaf();

    BranchTipInfo alice_tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    // Bob imports Alice's path nodes (branch is empty, no tip_block)
    EXPECT_NO_THROW(bob_->ms->import_partner_data(alice_tip));

    // Alice's path nodes are now in Bob's storage under Alice's user_id
    for (const Node& node : alice_tip.path)
        EXPECT_TRUE(bob_->storage->has_node(alice_->root_kp.pub, node.index));

    // No external block stored (empty branch)
    EXPECT_FALSE(bob_->storage->has_external_block(
        {alice_->root_kp.pub, LEAF, 0}));
}

TEST_F(MergeSessionTest, ImportPartnerDataWithBlock) {
    KeyPair alice_leaf = alice_->setup_leaf();
    bob_->setup_leaf();

    alice_->bc->append_data_block(
        alice_->root_kp.pub, LEAF, {0xCA, 0xFE}, alice_leaf, 1'000LL);

    BranchTipInfo alice_tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);
    ASSERT_TRUE(alice_tip.tip_block.has_value());

    bob_->ms->import_partner_data(alice_tip);

    // Path nodes imported
    for (const Node& node : alice_tip.path)
        EXPECT_TRUE(bob_->storage->has_node(alice_->root_kp.pub, node.index));

    // Tip block stored in external_blocks
    EXPECT_TRUE(bob_->storage->has_external_block(alice_tip.tip_address));
    Block stored = bob_->storage->get_external_block(alice_tip.tip_address);
    EXPECT_EQ(stored.address, alice_tip.tip_address);
}

TEST_F(MergeSessionTest, ImportPartnerDataIdempotent) {
    KeyPair alice_leaf = alice_->setup_leaf();
    bob_->setup_leaf();

    alice_->bc->append_data_block(
        alice_->root_kp.pub, LEAF, {0x42}, alice_leaf, 1'000LL);

    BranchTipInfo alice_tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    // Calling twice must not throw
    EXPECT_NO_THROW(bob_->ms->import_partner_data(alice_tip));
    EXPECT_NO_THROW(bob_->ms->import_partner_data(alice_tip));
}

// ── DAG snapshot accumulation across merges (§6.5) ────────────────────────────

TEST_F(MergeSessionTest, DagSnapshotAccumulatesUniqueParticipants) {
    auto carol = std::make_unique<UserCtx>(base_dir_, 3);

    KeyPair alice_leaf = alice_->setup_leaf();
    KeyPair bob_leaf   = bob_->setup_leaf();
    KeyPair carol_leaf = carol->setup_leaf();
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0xAA}, alice_leaf, 1'000LL);
    bob_->bc->append_data_block(  bob_->root_kp.pub,   LEAF, {0xBB}, bob_leaf,   1'000LL);
    carol->bc->append_data_block( carol->root_kp.pub,  LEAF, {0xCC}, carol_leaf, 1'000LL);

    // Alice ⋈ Bob → Alice's snapshot covers {A, B}
    BranchTipInfo bob_tip = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(bob_tip);
    MergeSnapshot bob_snap = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);
    alice_->ms->create_pending(alice_->root_kp.pub, LEAF, bob_tip, bob_snap,
                               alice_leaf, 2'000LL, 1u);
    EXPECT_EQ(alice_->ms->snapshot_for(alice_->root_kp.pub, LEAF).estimate(), 2u);

    // Alice ⋈ Carol → Alice's snapshot grows to {A, B, C}
    BranchTipInfo carol_tip = carol->ms->prepare_tip(carol->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(carol_tip);
    MergeSnapshot carol_snap = carol->ms->snapshot_for(carol->root_kp.pub, LEAF);
    alice_->ms->create_pending(alice_->root_kp.pub, LEAF, carol_tip, carol_snap,
                               alice_leaf, 3'000LL, 1u);
    EXPECT_EQ(alice_->ms->snapshot_for(alice_->root_kp.pub, LEAF).estimate(), 3u);

    carol.reset();
}

// ── merge precondition: both branches must be non-empty (§6.4) ─────────────────

TEST_F(MergeSessionTest, CreatePendingEmptyOwnBranchThrows) {
    KeyPair alice_leaf = alice_->setup_leaf();
    KeyPair bob_leaf   = bob_->setup_leaf();
    bob_->bc->append_data_block(bob_->root_kp.pub, LEAF, {0x02}, bob_leaf, 1'000LL);

    BranchTipInfo bob_tip = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(bob_tip);
    MergeSnapshot bob_snap = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);

    // Alice's branch is empty → cannot merge.
    EXPECT_THROW(
        alice_->ms->create_pending(alice_->root_kp.pub, LEAF, bob_tip, bob_snap,
                                   alice_leaf, 1'000LL, 1u),
        InvalidArgumentError);
}

TEST_F(MergeSessionTest, CreatePendingEmptyPartnerThrows) {
    KeyPair alice_leaf = alice_->setup_leaf();
    bob_->setup_leaf();
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0x01}, alice_leaf, 1'000LL);

    // Bob's branch is empty.
    BranchTipInfo bob_tip = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(bob_tip);

    // Empty partner → thrown before partner_snapshot is used (pass a default).
    EXPECT_THROW(
        alice_->ms->create_pending(alice_->root_kp.pub, LEAF, bob_tip, MergeSnapshot{},
                                   alice_leaf, 1'000LL, 1u),
        InvalidArgumentError);
}

TEST_F(MergeSessionTest, FinalizeWrongCoSigThrows) {
    KeyPair alice_leaf = alice_->setup_leaf();
    KeyPair bob_leaf   = bob_->setup_leaf();
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0x01}, alice_leaf, 1'000LL);
    bob_->bc->append_data_block(bob_->root_kp.pub, LEAF, {0x02}, bob_leaf, 1'000LL);

    BranchTipInfo bob_tip = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(bob_tip);
    MergeSnapshot bob_snap = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);

    PendingMergeBlock pending = alice_->ms->create_pending(
        alice_->root_kp.pub, LEAF, bob_tip, bob_snap, alice_leaf, 1'000LL, 1u);

    // Bad co-signature: corrupted
    Signature bad_co = Crypto::sign(
        pending.draft_hash.bytes.data(), 32, bob_leaf.sec);
    bad_co.bytes[0] ^= 0xFF;

    EXPECT_THROW(alice_->ms->finalize(pending, bad_co, bob_leaf.pub), SignatureError);
}
