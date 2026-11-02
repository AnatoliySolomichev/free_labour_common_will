#include "blockchain/merge_session.h"
#include "blockchain/key_path.h"
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
    EXPECT_EQ(tip.path.root.index, 0u);
    EXPECT_EQ(KeyPathCert::node(tip.path), LEAF);
    EXPECT_FALSE(tip.tip_block.has_value());

    // tip_hash for an empty branch: where its block #0 will grow from — the birth
    Node leaf = alice_->storage->get_node(alice_->root_kp.pub, LEAF);
    EXPECT_EQ(tip.tip_hash, leaf.birth.hash);
    EXPECT_EQ(tip.tip_hash, branch_origin(leaf));
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

    // Corrupt the root's self-signature
    tip.path.root.root_sig.bytes[0] ^= 0xFF;
    EXPECT_THROW(bob_->ms->verify_partner_tip(tip), SignatureError);
}

TEST_F(MergeSessionTest, VerifyPartnerTipMissingBirth) {
    alice_->setup_leaf();
    BranchTipInfo tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    // Drop an intermediate birth: the path no longer links parent → child
    tip.path.births.erase(tip.path.births.begin() + 1);
    EXPECT_THROW(bob_->ms->verify_partner_tip(tip), ChainIntegrityError);
}

// A path to another branch than the tip's does not pass.
TEST_F(MergeSessionTest, VerifyPartnerTipPathToAnotherBranch) {
    alice_->setup_leaf();
    BranchTipInfo tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);
    tip.path.births.pop_back();
    EXPECT_THROW(bob_->ms->verify_partner_tip(tip), ChainIntegrityError);
}

TEST_F(MergeSessionTest, VerifyPartnerTipEmptyPathThrows) {
    BranchTipInfo tip{};
    EXPECT_THROW(bob_->ms->verify_partner_tip(tip), ChainIntegrityError);
}

// ── Full bilateral merge protocol (blockchain.md §6.4) ──────────────────────────────────────

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

    // Step 2: Alice writes her half (snapshots exchanged first): it is signed
    // and in her branch at once.
    Timestamp ts = 2'000LL;
    MergeSnapshot alice_snap = alice_->ms->snapshot_for(alice_->root_kp.pub, LEAF);
    MergeSnapshot bob_snap   = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);
    const MergeSnapshot merged = MergeSnapshot::merge(alice_snap, bob_snap);

    const Block alice_half =
        alice_->ms->write_half(alice_tip, bob_tip, bob_snap, alice_leaf, ts, 1u);
    EXPECT_TRUE(alice_->storage->has_block(alice_half.address));

    // Step 3: Bob reads it and answers: his half takes hers itself.
    EXPECT_NO_THROW(bob_->ms->check_partner_half(alice_half, bob_tip, alice_tip, ts, merged));
    const BranchTipInfo alice_after = MergeSession::tip_after(alice_tip, alice_half);
    const Block bob_half =
        bob_->ms->write_half(bob_tip, alice_after, alice_snap, bob_leaf, ts, 1u);
    EXPECT_TRUE(bob_->storage->has_block(bob_half.address));

    // Alice reads the answer against her half.
    EXPECT_NO_THROW(alice_->ms->check_partner_half(bob_half, alice_after, bob_tip, ts, merged));

    const MergePayload alice_mp = Serializer::decode_merge_payload(
        alice_half.payload.data(), alice_half.payload.size());
    const MergePayload bob_mp = Serializer::decode_merge_payload(
        bob_half.payload.data(), bob_half.payload.size());
    // Hers takes his tip, his takes her half: each signature stands in its own
    // branch, right after the tip the other side named.
    EXPECT_EQ(alice_mp.partner_last_address, bob_tip.tip_address);
    EXPECT_EQ(alice_mp.partner_last_hash,    bob_tip.tip_hash);
    EXPECT_EQ(bob_mp.partner_last_address,   alice_half.address);
    EXPECT_EQ(bob_mp.partner_last_hash,      Crypto::hash_block(alice_half));
    EXPECT_EQ(bob_half.prev_hash,            bob_tip.tip_hash);

    // Both sides committed the same union snapshot (commutative merge).
    EXPECT_EQ(alice_mp.merkle_root, bob_mp.merkle_root);
    EXPECT_EQ(alice_mp.hll_hash,    bob_mp.hll_hash);
}

// ── import_partner_data ───────────────────────────────────────────────────────

TEST_F(MergeSessionTest, ImportPartnerDataEmptyBranch) {
    alice_->setup_leaf();
    bob_->setup_leaf();

    BranchTipInfo alice_tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);

    // Bob imports Alice's path nodes (branch is empty, no tip_block)
    EXPECT_NO_THROW(bob_->ms->import_partner_data(alice_tip));

    // Alice's path nodes are now in Bob's storage under Alice's user_id
    for (const Node& node : KeyPathCert::nodes(alice_tip.path))
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
    for (const Node& node : KeyPathCert::nodes(alice_tip.path))
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

// ── DAG snapshot accumulation across merges (blockchain.md §6.5) ────────────────────────────

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
    alice_->ms->write_half(alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF), bob_tip, bob_snap,
                           alice_leaf, 2'000LL, 1u);
    EXPECT_EQ(alice_->ms->snapshot_for(alice_->root_kp.pub, LEAF).estimate(), 2u);

    // Alice ⋈ Carol → Alice's snapshot grows to {A, B, C}
    BranchTipInfo carol_tip = carol->ms->prepare_tip(carol->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(carol_tip);
    MergeSnapshot carol_snap = carol->ms->snapshot_for(carol->root_kp.pub, LEAF);
    alice_->ms->write_half(alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF), carol_tip,
                           carol_snap, alice_leaf, 3'000LL, 1u);
    EXPECT_EQ(alice_->ms->snapshot_for(alice_->root_kp.pub, LEAF).estimate(), 3u);

    carol.reset();
}

// ── merge precondition: both branches must be non-empty (blockchain.md §6.4) ─────────────────

TEST_F(MergeSessionTest, WriteHalfEmptyOwnBranchThrows) {
    KeyPair alice_leaf = alice_->setup_leaf();
    KeyPair bob_leaf   = bob_->setup_leaf();
    bob_->bc->append_data_block(bob_->root_kp.pub, LEAF, {0x02}, bob_leaf, 1'000LL);

    BranchTipInfo bob_tip = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(bob_tip);
    MergeSnapshot bob_snap = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);

    // Alice's branch is empty → cannot merge.
    EXPECT_THROW(
        alice_->ms->write_half(alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF), bob_tip,
                               bob_snap, alice_leaf, 1'000LL, 1u),
        InvalidArgumentError);
}

TEST_F(MergeSessionTest, WriteHalfEmptyPartnerThrows) {
    KeyPair alice_leaf = alice_->setup_leaf();
    bob_->setup_leaf();
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0x01}, alice_leaf, 1'000LL);

    // Bob's branch is empty.
    BranchTipInfo bob_tip = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    alice_->ms->verify_partner_tip(bob_tip);

    // Empty partner → thrown before partner_snapshot is used (pass a default).
    EXPECT_THROW(
        alice_->ms->write_half(alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF), bob_tip,
                               MergeSnapshot{}, alice_leaf, 1'000LL, 1u),
        InvalidArgumentError);
}

// Половина встаёт сразу за вершиной, которую взял партнёр (blockchain.md §6.4):
// ветка, ушедшая дальше, — уже не та вершина.
TEST_F(MergeSessionTest, ABranchThatMovedOnWritesNoHalf) {
    KeyPair alice_leaf = alice_->setup_leaf();
    KeyPair bob_leaf   = bob_->setup_leaf();
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0x01}, alice_leaf, 1'000LL);
    bob_->bc->append_data_block(bob_->root_kp.pub, LEAF, {0x02}, bob_leaf, 1'000LL);

    const BranchTipInfo alice_tip = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);
    const BranchTipInfo bob_tip   = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    const MergeSnapshot bob_snap  = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0x03}, alice_leaf, 1'500LL);

    EXPECT_THROW(alice_->ms->write_half(alice_tip, bob_tip, bob_snap, alice_leaf, 2'000LL, 1u),
                 ChainIntegrityError);
}

// ── Время слияния (blockchain.md §9.4) ────────────────────────────────────────

// С вершиной из будущего не сливаются: ветка ждёт, пока время её догонит.
TEST_F(MergeSessionTest, ATipFromTheFutureIsNotMergedWith) {
    alice_->setup_leaf();
    KeyPair bob_leaf = bob_->setup_leaf();
    bob_->bc->append_data_block(bob_->root_kp.pub, LEAF, {0x02}, bob_leaf, 10'000LL);
    const BranchTipInfo bob_tip = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    EXPECT_NO_THROW(alice_->ms->check_partner_time(bob_tip, 10'000LL));
    EXPECT_NO_THROW(alice_->ms->check_partner_time(bob_tip, 10'000LL - kClockSlack));
    EXPECT_THROW(alice_->ms->check_partner_time(bob_tip, 10'000LL - kClockSlack - 1),
                 TimestampError);
}

// Ответчик читает половину инициатора, прежде чем ответить: она берёт посланную
// вершину, датирована «сейчас» по его часам и коммитит то же объединение снимков.
// Инициатор так же читает ответ. Иначе — не отвечать и ответа не принимать.
TEST_F(MergeSessionTest, AnAnswerIsWrittenOnlyAfterReadingTheHalf) {
    KeyPair alice_leaf = alice_->setup_leaf();
    KeyPair bob_leaf   = bob_->setup_leaf();
    alice_->bc->append_data_block(alice_->root_kp.pub, LEAF, {0xAA}, alice_leaf, 1'000LL);
    bob_->bc->append_data_block(bob_->root_kp.pub, LEAF, {0xBB}, bob_leaf, 1'000LL);
    const BranchTipInfo alice_tip  = alice_->ms->prepare_tip(alice_->root_kp.pub, LEAF);
    const BranchTipInfo bob_tip    = bob_->ms->prepare_tip(bob_->root_kp.pub, LEAF);
    const MergeSnapshot alice_snap = alice_->ms->snapshot_for(alice_->root_kp.pub, LEAF);
    const MergeSnapshot bob_snap   = bob_->ms->snapshot_for(bob_->root_kp.pub, LEAF);
    const MergeSnapshot merged     = MergeSnapshot::merge(alice_snap, bob_snap);
    const Block half =
        alice_->ms->write_half(alice_tip, bob_tip, bob_snap, alice_leaf, 2'000LL, 1u);

    // Алиса честна: в пределах допуска часов, берёт вершину Боба, то же объединение.
    EXPECT_NO_THROW(bob_->ms->check_partner_half(half, bob_tip, alice_tip, 2'000LL + 300, merged));
    // Часы Боба ушли на час вперёд: половина Алисы для него — из прошлого.
    EXPECT_THROW(bob_->ms->check_partner_half(half, bob_tip, alice_tip, 2'000LL + 3'600, merged),
                 TimestampError);
    // Половина, которая берёт не ту вершину, — не отвечать.
    BranchTipInfo other = bob_tip;
    other.tip_hash.bytes[0] ^= 0xFF;
    EXPECT_THROW(bob_->ms->check_partner_half(half, other, alice_tip, 2'000LL, merged),
                 ChainIntegrityError);
    // Другое объединение снимков — не то слияние.
    EXPECT_THROW(bob_->ms->check_partner_half(half, bob_tip, alice_tip, 2'000LL, bob_snap),
                 ChainIntegrityError);
    // Блок без слияния — не слияние.
    Block data = half;
    data.type = BlockType::DATA;
    EXPECT_THROW(bob_->ms->check_partner_half(data, bob_tip, alice_tip, 2'000LL, merged),
                 ChainIntegrityError);

    // Ответ, взявший вершину Алисы вместо её половины, ей ни о чём не говорит.
    const Block stale = bob_->ms->write_half(bob_tip, alice_tip, alice_snap, bob_leaf, 2'000LL, 1u);
    EXPECT_THROW(alice_->ms->check_partner_half(stale, MergeSession::tip_after(alice_tip, half),
                                                bob_tip, 2'000LL, merged),
                 ChainIntegrityError);
}
