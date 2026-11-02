#include "blockchain/blockchain.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <map>

using namespace blockchain;

// ── Fixture ───────────────────────────────────────────────────────────────────

// Leftmost leaf at depth 31: the first index with node_depth == 31.
// Depth d starts at index 2^d - 1.  For d=31: 2^31 - 1 = 0x7FFFFFFF.
static constexpr NodeIndex LEAF = 0x7FFF'FFFFu;

class BlockchainTest : public ::testing::Test {
protected:
    std::filesystem::path db_path_;
    std::unique_ptr<LmdbStorage> storage_;
    std::unique_ptr<Validator>   validator_;
    std::unique_ptr<Blockchain>  bc_;

    KeyPair                      root_kp_;
    std::map<NodeIndex, KeyPair> path_keys_; // keypairs for nodes on path to LEAF

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_bc_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_   = std::make_unique<LmdbStorage>(db_path_);
        validator_ = std::make_unique<Validator>(*storage_);
        bc_        = std::make_unique<Blockchain>(*storage_, *validator_);

        root_kp_        = Crypto::generate_keypair();
        path_keys_[0]   = root_kp_;
        bc_->create_identity(root_kp_);
    }

    void TearDown() override {
        bc_.reset(); validator_.reset(); storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    // Pre-generate keypairs for all nodes on the path to LEAF and call ensure_path.
    KeyPair setup_leaf_path() {
        for (NodeIndex idx : path_indices(LEAF))
            if (path_keys_.find(idx) == path_keys_.end())
                path_keys_[idx] = Crypto::generate_keypair();

        auto key_for = [&](NodeIndex idx) -> KeyPair {
            return path_keys_.at(idx);
        };
        bc_->ensure_path(root_kp_.pub, LEAF, key_for);
        return path_keys_.at(LEAF);
    }
};

// ── create_identity ───────────────────────────────────────────────────────────

TEST_F(BlockchainTest, CreateIdentityStoresRoot) {
    EXPECT_TRUE(storage_->has_node(root_kp_.pub, 0));
    Node n = storage_->get_node(root_kp_.pub, 0);
    EXPECT_EQ(n.index, 0u);
    EXPECT_EQ(n.structural_pubkey, root_kp_.pub);
    EXPECT_EQ(n.birth, BirthRef{});   // the root is born of no one
}

TEST_F(BlockchainTest, CreateIdentityRootSelfSigValid) {
    EXPECT_NO_THROW(validator_->validate_node(
        storage_->get_node(root_kp_.pub, 0), root_kp_.pub));
}

TEST_F(BlockchainTest, CreateIdentityDuplicateThrows) {
    EXPECT_THROW(bc_->create_identity(root_kp_), InvalidArgumentError);
}

// ── ensure_path ───────────────────────────────────────────────────────────────

TEST_F(BlockchainTest, EnsurePathCreatesAllNodes) {
    setup_leaf_path();
    for (NodeIndex idx : path_indices(LEAF))
        EXPECT_TRUE(storage_->has_node(root_kp_.pub, idx));
}

TEST_F(BlockchainTest, EnsurePathSignaturesValid) {
    setup_leaf_path();
    EXPECT_NO_THROW(bc_->validate_path(root_kp_.pub, LEAF));
}

TEST_F(BlockchainTest, EnsurePathIdempotent) {
    setup_leaf_path();
    // Calling again must not throw (all nodes already exist → no-op).
    auto key_for = [&](NodeIndex idx) -> KeyPair { return path_keys_.at(idx); };
    EXPECT_NO_THROW(bc_->ensure_path(root_kp_.pub, LEAF, key_for));
}

// Branches may grow from any node (blockchain.md §3.2 v0.7): a mid-depth node
// carries both child nodes and its own linear branch.
TEST_F(BlockchainTest, BranchOnIntermediateNode) {
    std::map<NodeIndex, KeyPair> keys;
    keys[0] = root_kp_;
    auto key_for = [&](NodeIndex idx) {
        auto it = keys.find(idx);
        if (it == keys.end()) it = keys.emplace(idx, Crypto::generate_keypair()).first;
        return it->second;
    };

    // Node 5 sits at depth 2 (path 0 → 2 → 5) — a "department" branch.
    bc_->ensure_path(root_kp_.pub, 5, key_for);
    const Block b0 = bc_->append_data_block(root_kp_.pub, 5, {0xAA},
                                            keys.at(5), 1'000LL);
    EXPECT_EQ(b0.address.node_index, 5u);
    EXPECT_EQ(b0.address.block_index, 0u);
    // Block 0 grows from the node's birth (blockchain.md §5.3).
    EXPECT_EQ(b0.prev_hash, storage_->get_node(root_kp_.pub, 5).birth.hash);

    // The same node still parents deeper structure: 5 → 11 works alongside.
    bc_->ensure_path(root_kp_.pub, 11, key_for);
    bc_->append_data_block(root_kp_.pub, 11, {0xBB}, keys.at(11), 1'001LL);

    // Its own block and the birth of 11 (blockchain.md §11.2).
    EXPECT_EQ(bc_->get_branch(root_kp_.pub, 5).size(), 2u);
    EXPECT_EQ(bc_->get_branch(root_kp_.pub, 11).size(), 1u);
}

TEST_F(BlockchainTest, EnsurePathInvalidIndexThrows) {
    auto key_for = [&](NodeIndex) { return Crypto::generate_keypair(); };
    // 0xFFFFFFFF sits at depth 32 — beyond the tree.
    EXPECT_THROW(bc_->ensure_path(root_kp_.pub, 0xFFFF'FFFFu, key_for),
                 InvalidArgumentError);
}

TEST_F(BlockchainTest, EnsurePathNoRootThrows) {
    KeyPair other = Crypto::generate_keypair();
    auto key_for  = [&](NodeIndex) { return Crypto::generate_keypair(); };
    EXPECT_THROW(bc_->ensure_path(other.pub, LEAF, key_for), NodeNotFoundError);
}

// ── get_path ─────────────────────────────────────────────────────────────────

TEST_F(BlockchainTest, GetPathLength) {
    setup_leaf_path();
    auto path = bc_->get_path(root_kp_.pub, LEAF);
    EXPECT_EQ(path.size(), path_indices(LEAF).size());
}

TEST_F(BlockchainTest, GetPathMissingNodeThrows) {
    // node 1 not stored → get_path to node 3 must throw
    EXPECT_THROW(bc_->get_path(root_kp_.pub, 3), NodeNotFoundError);
}

// ── branch_tip_hash ───────────────────────────────────────────────────────────

TEST_F(BlockchainTest, BranchTipHashEmptyBranchIsItsBirth) {
    setup_leaf_path();
    Node leaf     = storage_->get_node(root_kp_.pub, LEAF);
    Hash expected = leaf.birth.hash;
    EXPECT_EQ(bc_->branch_tip_hash(root_kp_.pub, LEAF), expected);
}

TEST_F(BlockchainTest, BranchTipHashAfterBlock) {
    KeyPair leaf_kp = setup_leaf_path();
    Block b = bc_->append_data_block(root_kp_.pub, LEAF, {0x01}, leaf_kp,
                                      1'000'000LL);
    Hash expected = Crypto::hash_block(b);
    EXPECT_EQ(bc_->branch_tip_hash(root_kp_.pub, LEAF), expected);
}

// ── append_data_block ─────────────────────────────────────────────────────────

TEST_F(BlockchainTest, AppendDataBlockFirstBlock) {
    KeyPair leaf_kp = setup_leaf_path();
    Block b = bc_->append_data_block(root_kp_.pub, LEAF, {0xAB}, leaf_kp,
                                      1'000'000LL);
    EXPECT_EQ(b.address.block_index, 0u);
    EXPECT_EQ(b.type, BlockType::DATA);
    EXPECT_EQ(b.payload, std::vector<uint8_t>({0xAB}));
}

TEST_F(BlockchainTest, AppendStubBlockBootstrapsBranch) {
    KeyPair leaf_kp = setup_leaf_path();
    // Empty branch: no tip yet.
    EXPECT_FALSE(storage_->branch_tip_index(root_kp_.pub, LEAF).has_value());

    Block b = bc_->append_stub_block(root_kp_.pub, LEAF, leaf_kp, 1'000LL);
    EXPECT_EQ(b.address.block_index, 0u);
    EXPECT_EQ(b.type, BlockType::DATA);
    EXPECT_EQ(b.payload, std::vector<uint8_t>({0x80}));  // empty CBOR array

    // Branch is now non-empty and validates.
    EXPECT_TRUE(storage_->branch_tip_index(root_kp_.pub, LEAF).has_value());
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, LEAF));
}

TEST_F(BlockchainTest, AppendStubBlockAsTimeAnchorTip) {
    KeyPair leaf_kp = setup_leaf_path();
    bc_->append_data_block(root_kp_.pub, LEAF, {0x01}, leaf_kp, 1'000LL);
    // A stub appended after existing work becomes the new tip (fresh anchor).
    Block anchor = bc_->append_stub_block(root_kp_.pub, LEAF, leaf_kp, 2'000LL);
    EXPECT_EQ(anchor.address.block_index, 1u);
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, LEAF));
}

TEST_F(BlockchainTest, AppendDataBlockSequentialIndex) {
    KeyPair leaf_kp = setup_leaf_path();
    Block b0 = bc_->append_data_block(root_kp_.pub, LEAF, {0x01}, leaf_kp, 1'000LL);
    Block b1 = bc_->append_data_block(root_kp_.pub, LEAF, {0x02}, leaf_kp, 2'000LL);
    Block b2 = bc_->append_data_block(root_kp_.pub, LEAF, {0x03}, leaf_kp, 3'000LL);
    EXPECT_EQ(b0.address.block_index, 0u);
    EXPECT_EQ(b1.address.block_index, 1u);
    EXPECT_EQ(b2.address.block_index, 2u);
}

TEST_F(BlockchainTest, AppendDataBlockPrevHashLinked) {
    KeyPair leaf_kp = setup_leaf_path();
    Node leaf = storage_->get_node(root_kp_.pub, LEAF);
    Block b0  = bc_->append_data_block(root_kp_.pub, LEAF, {0x01}, leaf_kp, 1'000LL);
    Block b1  = bc_->append_data_block(root_kp_.pub, LEAF, {0x02}, leaf_kp, 2'000LL);
    EXPECT_EQ(b0.prev_hash, leaf.birth.hash);
    EXPECT_EQ(b1.prev_hash, Crypto::hash_block(b0));
}

TEST_F(BlockchainTest, AppendDataBlockInvalidIndexThrows) {
    // 0xFFFFFFFF sits at depth 32 — beyond the tree. (Node 0 is fine now:
    // even the root has its own branch, blockchain.md §3.2 v0.7.)
    KeyPair kp = Crypto::generate_keypair();
    EXPECT_THROW(
        bc_->append_data_block(root_kp_.pub, 0xFFFF'FFFFu, {}, kp, 1'000LL),
        InvalidArgumentError);
}

TEST_F(BlockchainTest, AppendDataBlockSignatureValid) {
    KeyPair leaf_kp = setup_leaf_path();
    bc_->append_data_block(root_kp_.pub, LEAF, {0x01}, leaf_kp, 1'000LL);
    bc_->append_data_block(root_kp_.pub, LEAF, {0x02}, leaf_kp, 2'000LL);
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, LEAF));
}

// ── get_branch ────────────────────────────────────────────────────────────────

TEST_F(BlockchainTest, GetBranchEmpty) {
    setup_leaf_path();
    EXPECT_TRUE(bc_->get_branch(root_kp_.pub, LEAF).empty());
}

TEST_F(BlockchainTest, GetBranchOrder) {
    KeyPair leaf_kp = setup_leaf_path();
    bc_->append_data_block(root_kp_.pub, LEAF, {0x01}, leaf_kp, 1'000LL);
    bc_->append_data_block(root_kp_.pub, LEAF, {0x02}, leaf_kp, 2'000LL);
    bc_->append_data_block(root_kp_.pub, LEAF, {0x03}, leaf_kp, 3'000LL);
    auto branch = bc_->get_branch(root_kp_.pub, LEAF);
    ASSERT_EQ(branch.size(), 3u);
    EXPECT_EQ(branch[0].address.block_index, 0u);
    EXPECT_EQ(branch[1].address.block_index, 1u);
    EXPECT_EQ(branch[2].address.block_index, 2u);
}

// ── rotate_key ────────────────────────────────────────────────────────────────

TEST_F(BlockchainTest, RotateKeyAndContinue) {
    KeyPair old_kp  = setup_leaf_path();
    KeyPair new_kp  = Crypto::generate_keypair();

    bc_->append_data_block(root_kp_.pub, LEAF, {0x01}, old_kp, 1'000LL);
    Block rot = bc_->rotate_key(root_kp_.pub, LEAF, old_kp, new_kp, 2'000LL);
    EXPECT_EQ(rot.type, BlockType::KEY_ROTATION);
    EXPECT_EQ(rot.payload.size(), 32u);

    // After rotation, subsequent blocks must use new key.
    bc_->append_data_block(root_kp_.pub, LEAF, {0x02}, new_kp, 3'000LL);

    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, LEAF));
}

// ── validate_branch (integration) ────────────────────────────────────────────

TEST_F(BlockchainTest, ValidateBranchFullPathAndBlocks) {
    KeyPair leaf_kp = setup_leaf_path();
    for (int i = 0; i < 5; ++i)
        bc_->append_data_block(root_kp_.pub, LEAF, {static_cast<uint8_t>(i)},
                                leaf_kp, static_cast<Timestamp>(i + 1) * 1000LL);
    EXPECT_NO_THROW(bc_->validate_path(root_kp_.pub, LEAF));
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, LEAF));
}

// ── revoke_node / validate_revocation / effective_revocation (blockchain.md §6.7) ──────────

// Fixture with the path 0 → 1 → 3 → 7 created (branches may grow from any
// node, v0.7). Node 7 plays the compromised branch; 3 — parent, 1 — grandparent.
class RevocationTest : public BlockchainTest {
protected:
    KeyPair kp1_, kp3_, kp7_;

    void SetUp() override {
        BlockchainTest::SetUp();
        kp1_ = Crypto::generate_keypair();
        kp3_ = Crypto::generate_keypair();
        kp7_ = Crypto::generate_keypair();
        std::map<NodeIndex, KeyPair> keys{
            {0, root_kp_}, {1, kp1_}, {3, kp3_}, {7, kp7_}};
        bc_->ensure_path(root_kp_.pub, 7,
                         [&](NodeIndex i) { return keys.at(i); });
    }

    // The point before block #0 of a branch: its birth (blockchain.md §5.3).
    Hash origin(NodeIndex node) const { return storage_->get_node(root_kp_.pub, node).birth.hash; }

    // The replaced branch takes its key: the replacement block right after the
    // line (blockchain.md §6.7 rule 9).
    Block take(NodeIndex node, const KeyPair& repl, Timestamp ts) {
        return bc_->take_replacement(root_kp_.pub, node, repl, ts);
    }

    // Hand-craft a signed REVOCATION block in author's branch (bypasses the
    // creation-time checks of revoke_node; for negative validation tests).
    Block craft_revocation(NodeIndex author, const KeyPair& author_kp,
                           const RevocationPayload& rp, Timestamp ts) {
        Hash prev = bc_->branch_tip_hash(root_kp_.pub, author);
        auto tip  = storage_->branch_tip_index(root_kp_.pub, author);
        BlockIndex idx = tip.has_value() ? (*tip + 1) : 0;

        Block b{};
        b.address           = {root_kp_.pub, author, idx};
        b.prev_hash         = prev;
        b.timestamp_claimed = ts;
        b.type              = BlockType::REVOCATION;
        b.payload           = Serializer::encode(rp);
        b.signature         = Signature::null();
        auto bytes          = Serializer::encode(b);
        b.signature         = Crypto::sign(bytes.data(), bytes.size(), author_kp.sec);
        return b;
    }
};

TEST_F(RevocationTest, EmergencyStopByParent) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);

    EXPECT_EQ(b.address.node_index, 3u);
    EXPECT_EQ(b.type, BlockType::REVOCATION);

    auto rp = Serializer::decode_revocation_payload(b.payload.data(), b.payload.size());
    EXPECT_EQ(rp.revoked_node_index, 7u);
    EXPECT_EQ(rp.revoked_pubkey, kp7_.pub);
    EXPECT_FALSE(rp.replacement_pubkey.has_value());

    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 3));
    EXPECT_NO_THROW(validator_->validate_revocation(b));
}

TEST_F(RevocationTest, RevokeByGrandparent) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 1, kp1_, 2'000LL);
    EXPECT_EQ(b.address.node_index, 1u);
    EXPECT_NO_THROW(validator_->validate_revocation(b));
}

TEST_F(RevocationTest, NonAncestorAuthorThrows) {
    // Node 4 is in a sibling subtree of 3 (path 0 → 1 → 4): not an ancestor of 7.
    KeyPair kp4 = Crypto::generate_keypair();
    std::map<NodeIndex, KeyPair> keys{{0, root_kp_}, {1, kp1_}, {4, kp4}};
    bc_->ensure_path(root_kp_.pub, 4, [&](NodeIndex i) { return keys.at(i); });

    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 4, kp4, 2'000LL),
        RevocationError);
}

TEST_F(RevocationTest, SelfRevocationThrows) {
    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 7, kp7_, 2'000LL),
        RevocationError);
}

TEST_F(RevocationTest, RootRevocationThrows) {
    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 0, std::nullopt, 0, root_kp_, 2'000LL),
        RevocationError);
}

TEST_F(RevocationTest, AncestorKeypairMismatchThrows) {
    // Signing with node 1's key while writing into node 3's branch.
    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp1_, 2'000LL),
        InvalidArgumentError);
}

TEST_F(RevocationTest, StopThenReplaceLastWordWins) {
    // Night: emergency stop. Later: a second block assigns the replacement.
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 5'100LL);
    KeyPair fresh = Crypto::generate_keypair();
    Block second = bc_->revoke_node(root_kp_.pub, 7, fresh.pub, 3, kp3_, 9'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    ASSERT_TRUE(st->replacement_pubkey.has_value());
    EXPECT_EQ(*st->replacement_pubkey, fresh.pub);
    EXPECT_EQ(st->latest, second.address);

    EXPECT_NO_THROW(validator_->validate_revocation(second));
}

TEST_F(RevocationTest, ALaterRecordHasTheLastWord) {
    // Stop first, replace later: the later block of the same author decides.
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 5'000LL);
    KeyPair fresh = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, fresh.pub, 3, kp3_, 9'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    ASSERT_TRUE(st->replacement_pubkey.has_value());
    EXPECT_EQ(*st->replacement_pubkey, fresh.pub);
}

TEST_F(RevocationTest, HigherAncestorWins) {
    KeyPair repl_parent = Crypto::generate_keypair();
    KeyPair repl_grand  = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl_parent.pub, 3, kp3_, 2'000LL);
    bc_->revoke_node(root_kp_.pub, 7, repl_grand.pub, 1, kp1_, 5'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    // Grandparent's branch decides, parent's replacement is ignored (blockchain.md §4.4).
    EXPECT_EQ(st->latest.node_index, 1u);
    ASSERT_TRUE(st->replacement_pubkey.has_value());
    EXPECT_EQ(*st->replacement_pubkey, repl_grand.pub);
}

TEST_F(RevocationTest, EffectiveRevocationNoneIsNullopt) {
    EXPECT_FALSE(validator_->effective_revocation(root_kp_.pub, 7).has_value());
}

TEST_F(RevocationTest, WrongRevokedPubkeyRejected) {
    RevocationPayload rp{};
    rp.revoked_node_index = 7;
    rp.revoked_pubkey     = Crypto::generate_keypair().pub; // never was node 7's key

    Block b = craft_revocation(3, kp3_, rp, 2'000LL);
    EXPECT_THROW(validator_->validate_revocation(b), RevocationError);
}

TEST_F(RevocationTest, RevokeAfterRotationUsesCurrentKey) {
    KeyPair rotated = Crypto::generate_keypair();
    bc_->rotate_key(root_kp_.pub, 7, kp7_, rotated, 1'000LL);

    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 3'000LL);
    auto rp = Serializer::decode_revocation_payload(b.payload.data(), b.payload.size());
    EXPECT_EQ(rp.revoked_pubkey, rotated.pub);
    EXPECT_NO_THROW(validator_->validate_revocation(b));
}

TEST_F(RevocationTest, HistoricalPreRotationKeyAccepted) {
    KeyPair rotated = Crypto::generate_keypair();
    bc_->rotate_key(root_kp_.pub, 7, kp7_, rotated, 1'000LL);

    // Revoking the old (pre-rotation) key is legal: it is a historical key.
    RevocationPayload rp{};
    rp.revoked_node_index = 7;
    rp.revoked_pubkey     = kp7_.pub;

    Block b = craft_revocation(3, kp3_, rp, 2'000LL);
    EXPECT_NO_THROW(validator_->validate_revocation(b));
}

TEST_F(RevocationTest, ReplacementKeyIsRevocableAgain) {
    // stop → replace → the replacement key itself gets compromised.
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'000LL);

    RevocationPayload rp{};
    rp.revoked_node_index = 7;
    rp.revoked_pubkey     = repl.pub; // known only via the earlier replacement

    Block b = craft_revocation(3, kp3_, rp, 4'000LL);
    storage_->put_block(b);
    EXPECT_NO_THROW(validator_->validate_revocation(b));

    // The last word: no new replacement → branch is frozen again.
    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    EXPECT_FALSE(st->replacement_pubkey.has_value());
}

// ── Step 2: revocation effects in validation (blockchain.md §6.7 rules 9–11) ────────────────

// Без названной линии — полный стоп: все блоки ключа под вопросом. Линия делит их
// по месту; дата в блоке ни при чём (blockchain.md §6.7 rule 12).
TEST_F(RevocationTest, AStopLeavesBlocksInQuestionUntilTheLine) {
    const Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 2'000LL);
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 3'000LL);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::FROZEN);
    EXPECT_FALSE(st.next_key.has_value());
    ASSERT_EQ(st.blocks.size(), 2u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::SUSPECT);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::SUSPECT);

    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 4'000LL, Crypto::hash_block(b0));
    st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::OUTSIDE_LINE);
}

// Ни одного верного блока: отзыв называет точку до блока №0 — отпечаток блока рождения.
TEST_F(RevocationTest, ALineCanEndBeforeTheFirstBlock) {
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    const Hash before = storage_->get_node(root_kp_.pub, 7).birth.hash;
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL, before);
    const auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 1u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::OUTSIDE_LINE);
    EXPECT_FALSE(st.line_end.has_value());
}

TEST_F(RevocationTest, ActiveBranchStatus) {
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::ACTIVE);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, kp7_.pub);
    ASSERT_EQ(st.blocks.size(), 1u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);
}

TEST_F(RevocationTest, AppendToFrozenBranchThrows) {
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);
    EXPECT_THROW(
        bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 3'000LL),
        RevocationError);
}

TEST_F(RevocationTest, ReplacedBranchAcceptsOnlyReplacement) {
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'000LL, origin(7));   // no valid block

    EXPECT_THROW(
        bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 3'000LL),
        RevocationError);
    // The replacement starts with its replacement block, nothing before it.
    EXPECT_THROW(
        bc_->append_data_block(root_kp_.pub, 7, {0x01}, repl, 3'000LL),
        RevocationError);
    const Block k = take(7, repl, 3'000LL);
    EXPECT_EQ(k.address.block_index, 0u);
    EXPECT_EQ(k.prev_hash, origin(7));          // right after the empty line: the birth

    Block b = bc_->append_data_block(root_kp_.pub, 7, {0x01}, repl, 3'000LL);
    EXPECT_EQ(b.address.block_index, 1u);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::REPLACED);
    EXPECT_FALSE(st.awaiting_replacement);
    ASSERT_EQ(st.blocks.size(), 2u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::CLEAN);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, repl.pub);

    // validate_branch follows the out-of-branch key switch (blockchain.md §6.7 rule 9).
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

// Стоп-кран с заменой: замена ждёт линию (blockchain.md §6.7 правило 9). Линию
// называют потом, вместе с той же заменой, — и замена встаёт сразу за ней.
TEST_F(RevocationTest, SwitchAfterExistingBlocks) {
    const Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'000LL);
    EXPECT_THROW(bc_->append_data_block(root_kp_.pub, 7, {0x02}, repl, 3'000LL),
                 RevocationError);
    EXPECT_THROW(take(7, repl, 2'500LL), RevocationError);          // no line yet
    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_TRUE(st.awaiting_replacement);
    EXPECT_FALSE(st.line.has_value());
    EXPECT_EQ(st.blocks.at(0), BlockRevocationStatus::SUSPECT);

    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'600LL, Crypto::hash_block(b0));
    take(7, repl, 3'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, repl, 3'000LL);

    st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 3u);
    for (auto s : st.blocks) EXPECT_EQ(s, BlockRevocationStatus::CLEAN);
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

TEST_F(RevocationTest, OldKeyAfterSwitchIsInvalid) {
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'000LL, origin(7));
    take(7, repl, 3'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, repl, 3'000LL);

    // The thief appends with the old key after the switch (bypassing the facade).
    Hash prev = bc_->branch_tip_hash(root_kp_.pub, 7);
    Block b{};
    b.address           = {root_kp_.pub, 7, 2};
    b.prev_hash         = prev;
    b.timestamp_claimed = 4'000LL;
    b.type              = BlockType::DATA;
    b.payload           = {0x80};
    b.signature         = Signature::null();
    auto bytes          = Serializer::encode(b);
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp7_.sec);
    storage_->put_block(b);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 3u);
    EXPECT_EQ(st.blocks[2], BlockRevocationStatus::INVALID_AFTER_REPLACEMENT);
    EXPECT_THROW(bc_->validate_branch(root_kp_.pub, 7), SignatureError);
}

TEST_F(RevocationTest, RotationAfterReplacementContinues) {
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'000LL, origin(7));
    take(7, repl, 3'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, repl, 3'000LL);

    KeyPair repl2 = Crypto::generate_keypair();
    bc_->rotate_key(root_kp_.pub, 7, repl, repl2, 4'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, repl2, 5'000LL);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::REPLACED);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, repl2.pub);
    for (auto s : st.blocks) EXPECT_EQ(s, BlockRevocationStatus::CLEAN);
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

TEST_F(RevocationTest, HijackedAuthorRecordIsFiltered) {
    // The thief of node 3's key "revokes" 7 to hijack it via replacement.
    KeyPair thief_repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, thief_repl.pub, 3, kp3_, 9'000LL);

    // The owner escalates: node 1 revokes node 3 as compromised since 5'000 —
    // earlier than the thief's record (ts 9'000), so that record loses weight
    // (blockchain.md §6.7 rule 10).
    bc_->revoke_node(root_kp_.pub, 3, std::nullopt, 1, kp1_, 10'000LL);

    EXPECT_FALSE(validator_->effective_revocation(root_kp_.pub, 7).has_value());
}

TEST_F(RevocationTest, AnAuthorRecordOnItsLineCounts) {
    // Under a full stop of the author its record is in question; on the line it counts.
    const Block record = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 3'000LL);
    bc_->revoke_node(root_kp_.pub, 3, std::nullopt, 1, kp1_, 10'000LL);
    EXPECT_FALSE(validator_->effective_revocation(root_kp_.pub, 7).has_value());

    bc_->revoke_node(root_kp_.pub, 3, std::nullopt, 1, kp1_, 11'000LL, Crypto::hash_block(record));
    EXPECT_TRUE(validator_->effective_revocation(root_kp_.pub, 7).has_value());
}

TEST_F(RevocationTest, ReplacedAuthorRevokesWithNewKey) {
    // Node 3 was revoked and replaced; the owner keeps administering 7 from 3 —
    // records signed by the replacement count (blockchain.md §6.7 rule 10).
    KeyPair repl3 = Crypto::generate_keypair();
    const Block line3 = storage_->get_block({root_kp_.pub, 3, 0});   // the birth of 7
    bc_->revoke_node(root_kp_.pub, 3, repl3.pub, 1, kp1_, 6'000LL, Crypto::hash_block(line3));
    // Before its replacement block the new key writes no record.
    EXPECT_THROW(bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, repl3, 7'000LL),
                 RevocationError);
    take(3, repl3, 7'000LL);
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, repl3, 8'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
}

TEST_F(RevocationTest, NodeInvalidatedByRevocation) {
    // Children of 7: 16 (on the owner's line) and 15 (past it).
    std::map<NodeIndex, KeyPair> kids{{7, kp7_}, {15, Crypto::generate_keypair()},
                                      {16, Crypto::generate_keypair()},
                                      {31, Crypto::generate_keypair()}};
    const auto key = [&](NodeIndex i) { return kids.at(i); };
    bc_->ensure_path(root_kp_.pub, 16, key, 1'000LL);
    bc_->ensure_path(root_kp_.pub, 15, key, 9'000LL);
    // Grandchild under the poisoned edge: an early date saves nothing.
    bc_->ensure_path(root_kp_.pub, 31, key, 100LL);

    const Hash line = Crypto::hash_block(storage_->get_block({root_kp_.pub, 7, 0}));
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 6'000LL, line);

    EXPECT_TRUE (validator_->node_invalidated_by_revocation(root_kp_.pub, 15));
    EXPECT_FALSE(validator_->node_invalidated_by_revocation(root_kp_.pub, 16));
    EXPECT_TRUE (validator_->node_invalidated_by_revocation(root_kp_.pub, 31));
    EXPECT_FALSE(validator_->node_invalidated_by_revocation(root_kp_.pub, 7));
}

TEST_F(RevocationTest, StorageIndexReturnsAddresses) {
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 1, kp1_, 3'000LL);

    auto addrs = storage_->get_revocation_addresses(root_kp_.pub, 7);
    ASSERT_EQ(addrs.size(), 2u);
    EXPECT_EQ(addrs[0].node_index, 3u);
    EXPECT_EQ(addrs[1].node_index, 1u);
    EXPECT_TRUE(storage_->get_revocation_addresses(root_kp_.pub, 15).empty());
}

// ── Step 3: self-verifying revocation certificate (blockchain.md §6.7 rule 8) ───────────────

TEST_F(RevocationTest, CertificateBuildAndVerify) {
    KeyPair repl = Crypto::generate_keypair();
    Block b = bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'000LL);

    auto cert = RevocationCert::build(*storage_, b.address);
    EXPECT_EQ(cert.path.births.size(), 2u); // nodes 0 → 1 → 3
    EXPECT_NO_THROW(RevocationCert::verify(cert));

    auto rp = RevocationCert::payload(cert);
    EXPECT_EQ(rp.revoked_node_index, 7u);
    EXPECT_EQ(rp.revoked_pubkey, kp7_.pub);
    ASSERT_TRUE(rp.replacement_pubkey.has_value());
    EXPECT_EQ(*rp.replacement_pubkey, repl.pub);
}

TEST_F(RevocationTest, CertificateSurvivesSerialization) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 1, kp1_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);

    auto enc = Serializer::encode(cert);
    auto dec = Serializer::decode_revocation_cert(enc.data(), enc.size());

    EXPECT_NO_THROW(RevocationCert::verify(dec));
    EXPECT_EQ(dec.path.births.size(), cert.path.births.size()); // grandparent author: 0 → 1
    EXPECT_EQ(Crypto::hash_block(dec.block), Crypto::hash_block(cert.block));
}

TEST_F(RevocationTest, CertificateTamperedNodeRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);
    cert.path.births[0].payload.back() ^= 0x01; // node 1's keys, not what the root signed
    EXPECT_THROW(RevocationCert::verify(cert), SignatureError);
}

TEST_F(RevocationTest, CertificateTruncatedPathRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);
    cert.path.births.pop_back(); // tip no longer matches the block's node
    EXPECT_THROW(RevocationCert::verify(cert), ChainIntegrityError);
}

TEST_F(RevocationTest, CertificateTamperedPayloadRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);
    cert.block.payload.push_back(0x00); // breaks the block signature
    EXPECT_THROW(RevocationCert::verify(cert), SignatureError);
}

TEST_F(RevocationTest, CertificateForeignRootRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);
    cert.block.address.user_id = Crypto::generate_keypair().pub;
    // Root identity no longer matches the block's chain (checked before sigs).
    EXPECT_THROW(RevocationCert::verify(cert), RevocationError);
}

TEST_F(RevocationTest, CertificateNonAncestorRejected) {
    // A "revocation" of node 7 authored by node 4 — a sibling subtree.
    KeyPair kp4 = Crypto::generate_keypair();
    std::map<NodeIndex, KeyPair> keys{{0, root_kp_}, {1, kp1_}, {4, kp4}};
    bc_->ensure_path(root_kp_.pub, 4, [&](NodeIndex i) { return keys.at(i); });

    RevocationPayload rp{};
    rp.revoked_node_index = 7;
    rp.revoked_pubkey     = kp7_.pub;
    Block b = craft_revocation(4, kp4, rp, 2'000LL);

    RevocationCertificate cert{};
    cert.block = b;
    cert.path  = KeyPathCert::build(*storage_, root_kp_.pub, 4);
    EXPECT_THROW(RevocationCert::verify(cert), RevocationError);
}

// ── Step 4: imported foreign revocations feed the local index (blockchain.md §6.7 rule 8) ───

TEST_F(RevocationTest, ImportedForeignRevocationIsEffective) {
    // A partner's chain lives in its own storage; we receive its revocation as
    // a certificate and import it: path nodes + external block → local index.
    auto foreign_db = std::filesystem::temp_directory_path() / "bc_foreign_rev";
    std::filesystem::remove_all(foreign_db);
    {
        LmdbStorage f_storage(foreign_db);
        Validator   f_validator(f_storage);
        Blockchain  f_bc(f_storage, f_validator);

        KeyPair f_root = Crypto::generate_keypair();
        KeyPair f_kp1  = Crypto::generate_keypair();
        KeyPair f_kp3  = Crypto::generate_keypair();
        KeyPair f_kp7  = Crypto::generate_keypair();
        std::map<NodeIndex, KeyPair> keys{
            {0, f_root}, {1, f_kp1}, {3, f_kp3}, {7, f_kp7}};
        f_bc.create_identity(f_root);
        f_bc.ensure_path(f_root.pub, 7, [&](NodeIndex i) { return keys.at(i); });

        KeyPair repl = Crypto::generate_keypair();
        Block b = f_bc.revoke_node(f_root.pub, 7, repl.pub, 3, f_kp3, 5'000LL);
        auto cert = RevocationCert::build(f_storage, b.address);

        // ── the "wire": only certificate bytes cross storages ──
        auto bytes = Serializer::encode(cert);
        auto recv  = Serializer::decode_revocation_cert(bytes.data(), bytes.size());
        RevocationCert::verify(recv);

        // Import into OUR storage (as bc revoke fetch does).
        const UserId chain = recv.block.address.user_id;
        for (const auto& node : KeyPathCert::nodes(recv.path))
            if (!storage_->has_node(chain, node.index))
                storage_->put_node(chain, node);
        storage_->put_external_block(recv.block);

        // The partner's node 7 is now revoked in our view.
        auto st = validator_->effective_revocation(chain, 7);
        ASSERT_TRUE(st.has_value());
        ASSERT_TRUE(st->replacement_pubkey.has_value());
        EXPECT_EQ(*st->replacement_pubkey, repl.pub);
    }
    std::filesystem::remove_all(foreign_db);
}

// ── Step 5: acceptance-time revocation checks (blockchain.md §6.7 rule 11) ──────────────────

// Partner chain living in its own storage; exposes tips the way sync would.
struct ForeignChain {
    std::filesystem::path        dir;
    std::unique_ptr<LmdbStorage> storage;
    std::unique_ptr<Validator>   validator;
    std::unique_ptr<Blockchain>  bc;
    KeyPair root, kp1, kp3, kp7;

    explicit ForeignChain(const std::string& tag) {
        dir = std::filesystem::temp_directory_path() / ("bc_foreign_" + tag);
        std::filesystem::remove_all(dir);
        storage   = std::make_unique<LmdbStorage>(dir);
        validator = std::make_unique<Validator>(*storage);
        bc        = std::make_unique<Blockchain>(*storage, *validator);
        root = Crypto::generate_keypair();
        kp1  = Crypto::generate_keypair();
        kp3  = Crypto::generate_keypair();
        kp7  = Crypto::generate_keypair();
        std::map<NodeIndex, KeyPair> keys{{0, root}, {1, kp1}, {3, kp3}, {7, kp7}};
        bc->create_identity(root);
        bc->ensure_path(root.pub, 7, [&](NodeIndex i) { return keys.at(i); });
    }
    ~ForeignChain() {
        bc.reset(); validator.reset(); storage.reset();
        std::filesystem::remove_all(dir);
    }

    UserId uid() const { return root.pub; }

    BranchTipInfo tip(NodeIndex branch) const {
        MergeSession s(*storage, *validator);
        return s.prepare_tip(root.pub, branch);
    }
};

// Import the certificate of a foreign REVOCATION block, as bc revoke fetch does.
static void import_certificate(LmdbStorage& into, const ForeignChain& f,
                               const Block& rev_block) {
    auto cert  = RevocationCert::build(*f.storage, rev_block.address);
    auto bytes = Serializer::encode(cert);
    auto recv  = Serializer::decode_revocation_cert(bytes.data(), bytes.size());
    RevocationCert::verify(recv);
    for (const auto& node : KeyPathCert::nodes(recv.path))
        if (!into.has_node(f.uid(), node.index))
            into.put_node(f.uid(), node);
    into.put_external_block(recv.block);
}

TEST_F(RevocationTest, TipCheckRefusesFrozenPartnerBranch) {
    ForeignChain f("frozen");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    Block rev = f.bc->revoke_node(f.uid(), 7, std::nullopt, 3, f.kp3, 2'000LL);
    import_certificate(*storage_, f, rev);

    EXPECT_THROW(validator_->check_tip_against_revocations(f.tip(7)),
                 RevocationError);
}

TEST_F(RevocationTest, TipCheckRefusesStaleTipOfReplacedBranch) {
    ForeignChain f("stale");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    KeyPair repl = Crypto::generate_keypair();
    Block rev = f.bc->revoke_node(f.uid(), 7, repl.pub, 3, f.kp3, 2'000LL);
    import_certificate(*storage_, f, rev);

    // The tip is still under the old key — stale or stolen.
    EXPECT_THROW(validator_->check_tip_against_revocations(f.tip(7)),
                 RevocationError);
}

TEST_F(RevocationTest, TipCheckAcceptsReplacedTip) {
    ForeignChain f("replaced");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    KeyPair repl = Crypto::generate_keypair();
    Block rev = f.bc->revoke_node(f.uid(), 7, repl.pub, 3, f.kp3, 2'000LL,
                                  f.bc->branch_tip_hash(f.uid(), 7));
    f.bc->take_replacement(f.uid(), 7, repl, 2'500LL);
    f.bc->append_data_block(f.uid(), 7, {0x02}, repl, 3'000LL); // re-tip under R
    import_certificate(*storage_, f, rev);

    EXPECT_NO_THROW(validator_->check_tip_against_revocations(f.tip(7)));
}

TEST_F(RevocationTest, TipCheckSilentWithoutLocalKnowledge) {
    ForeignChain f("unknown");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    f.bc->revoke_node(f.uid(), 7, std::nullopt, 3, f.kp3, 2'000LL);
    // Certificate NOT imported: no local knowledge — no objection.
    // Keeping knowledge fresh is the caller's policy (sync.md §10.3).
    EXPECT_NO_THROW(validator_->check_tip_against_revocations(f.tip(7)));
}

TEST_F(RevocationTest, TipCheckRefusesFakeSubtreeNode) {
    ForeignChain f("fake");
    // Node 15 under 7, born after the compromise moment (blockchain.md §6.7 rule 6).
    KeyPair kp15 = Crypto::generate_keypair();
    f.bc->ensure_path(f.uid(), 15,
                      [&](NodeIndex i) { return i == 15 ? kp15 : f.kp7; }, 9'000LL);
    f.bc->append_data_block(f.uid(), 15, {0x01}, kp15, 9'500LL);

    Block rev = f.bc->revoke_node(f.uid(), 7, std::nullopt, 3, f.kp3, 9'600LL);
    import_certificate(*storage_, f, rev);

    // Branch 15 itself is not revoked; the poisoned edge 7→15 is refused.
    EXPECT_THROW(validator_->check_tip_against_revocations(f.tip(15)),
                 RevocationError);
}

TEST_F(RevocationTest, VerifyPartnerTipChecksRevocations) {
    ForeignChain f("verify");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    Block rev = f.bc->revoke_node(f.uid(), 7, std::nullopt, 3, f.kp3, 2'000LL);
    import_certificate(*storage_, f, rev);

    MergeSession session(*storage_, *validator_);
    EXPECT_THROW(session.verify_partner_tip(f.tip(7)), RevocationError);
}

TEST_F(RevocationTest, MergeFromRevokedOwnBranchRefused) {
    // A frozen own branch may not open new bilateral acts (merges bypass the
    // facade's append protection).
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);

    ForeignChain f("mergepeer");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    MergeSession f_session(*f.storage, *f.validator);
    auto partner_tip  = f.tip(7);
    auto partner_snap = f_session.snapshot_for(f.uid(), 7);

    MergeSession session(*storage_, *validator_);
    EXPECT_THROW(
        session.write_half(session.prepare_tip(root_kp_.pub, 7), partner_tip, partner_snap,
                           kp7_, 3'000LL, 1),
        RevocationError);
}


// ── Repeated thefts and the owner's line (blockchain.md §6.7 rules 9, 10, 12) ────────────────

TEST_F(RevocationTest, SecondTheftOfTheSameLeaf) {
    const Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    KeyPair k1 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, k1.pub, 3, kp3_, 2'000LL, Crypto::hash_block(b0));
    take(7, k1, 2'500LL);
    const Block b1 = bc_->append_data_block(root_kp_.pub, 7, {0x02}, k1, 3'000LL);

    // The replacement is stolen too: the parent revokes it in turn.
    KeyPair k2 = Crypto::generate_keypair();
    Block r2 = bc_->revoke_node(root_kp_.pub, 7, k2.pub, 3, kp3_, 4'000LL, Crypto::hash_block(b1));
    auto rp2 = Serializer::decode_revocation_payload(r2.payload.data(), r2.payload.size());
    EXPECT_EQ(rp2.revoked_pubkey, k1.pub);
    EXPECT_NO_THROW(validator_->validate_revocation(r2));

    EXPECT_THROW(bc_->append_data_block(root_kp_.pub, 7, {0x03}, k1, 5'000LL),
                 RevocationError);
    take(7, k2, 4'500LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x03}, k2, 5'000LL);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::REPLACED);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, k2.pub);
    ASSERT_EQ(st.blocks.size(), 5u);
    for (auto s : st.blocks) EXPECT_EQ(s, BlockRevocationStatus::CLEAN);   // both lines, then K2
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

// Замена, не начавшаяся в ветке, — не эпоха: новая запись о той же ветке
// говорит о ключе в силе и перекрывает её (последнее слово, правило 3).
TEST_F(RevocationTest, ReplacementRevokedBeforeUseIsSkipped) {
    KeyPair k1 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, k1.pub, 3, kp3_, 1'000LL, origin(7));
    KeyPair k2 = Crypto::generate_keypair();
    Block r2 = bc_->revoke_node(root_kp_.pub, 7, k2.pub, 3, kp3_, 2'000LL);
    auto rp2 = Serializer::decode_revocation_payload(r2.payload.data(), r2.payload.size());
    EXPECT_EQ(rp2.revoked_pubkey, kp7_.pub);          // the epoch in force, not the unused K1
    ASSERT_TRUE(rp2.last_good.has_value());           // and its line goes along
    EXPECT_EQ(rp2.last_good->hash, origin(7));

    EXPECT_THROW(take(7, k1, 3'000LL), RevocationError);
    take(7, k2, 3'000LL);
    EXPECT_THROW(bc_->append_data_block(root_kp_.pub, 7, {0x01}, k1, 3'000LL),
                 RevocationError);
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, k2, 3'000LL);
    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::REPLACED);
    ASSERT_EQ(st.blocks.size(), 2u);
    for (auto s : st.blocks) EXPECT_EQ(s, BlockRevocationStatus::CLEAN);
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

TEST_F(RevocationTest, EarlierEpochKeyAfterSecondSwitchIsInvalid) {
    KeyPair k1 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, k1.pub, 3, kp3_, 1'000LL, origin(7));
    take(7, k1, 1'500LL);
    const Block b1 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, k1, 2'000LL);
    KeyPair k2 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, k2.pub, 3, kp3_, 3'000LL, Crypto::hash_block(b1));
    take(7, k2, 3'500LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, k2, 4'000LL);

    // The thief of K1 appends past the switch to K2 (bypassing the facade).
    Block b{};
    b.address           = {root_kp_.pub, 7, 4};
    b.prev_hash         = bc_->branch_tip_hash(root_kp_.pub, 7);
    b.timestamp_claimed = 5'000LL;
    b.type              = BlockType::DATA;
    b.payload           = {0x80};
    b.signature         = Signature::null();
    auto bytes          = Serializer::encode(b);
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), k1.sec);
    storage_->put_block(b);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 5u);
    EXPECT_EQ(st.blocks[4], BlockRevocationStatus::INVALID_AFTER_REPLACEMENT);
    EXPECT_THROW(bc_->validate_branch(root_kp_.pub, 7), SignatureError);
}

TEST_F(RevocationTest, LastGoodSplitsByLineNotByTime) {
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    Block b1 = bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 2'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x03}, kp7_, 3'000LL);

    // Suspected since long ago, but the owner names B1 as the last own block.
    KeyPair k1 = Crypto::generate_keypair();
    Block r = bc_->revoke_node(root_kp_.pub, 7, k1.pub, 3, kp3_, 4'000LL,
                               Crypto::hash_block(b1));
    auto rp = Serializer::decode_revocation_payload(r.payload.data(), r.payload.size());
    ASSERT_TRUE(rp.last_good.has_value());
    EXPECT_EQ(rp.last_good->index, 1u);
    EXPECT_EQ(rp.last_good->hash, Crypto::hash_block(b1));

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 3u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(st.blocks[2], BlockRevocationStatus::OUTSIDE_LINE);
    ASSERT_TRUE(st.line_end.has_value());
    EXPECT_EQ(*st.line_end, 1u);
    ASSERT_TRUE(st.revocation.has_value());
    ASSERT_TRUE(st.revocation->last_good.has_value());

    // The replacement continues right after the line, not past it: this copy
    // holds a block after B1, so its replacement block has no place here
    // (решение участника 2026-11-02).
    EXPECT_THROW(take(7, k1, 5'000LL), RevocationError);
    EXPECT_THROW(bc_->append_data_block(root_kp_.pub, 7, {0x04}, k1, 5'000LL), RevocationError);
}

TEST_F(RevocationTest, LastGoodMustBeABlockOfTheBranch) {
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    Hash stranger{};
    stranger.bytes.fill(0x5A);
    EXPECT_THROW(bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL,
                                  stranger),
                 InvalidArgumentError);
}

TEST_F(RevocationTest, LastGoodNamedLaterWithTheReplacement) {
    Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 2'000LL);

    // Stop first, in a hurry: zones by time.
    bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 3'000LL);
    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::FROZEN);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::SUSPECT);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::SUSPECT);

    // Later, calmly: the replacement and the owner's line (rule 3, rule 12).
    KeyPair k1 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, k1.pub, 3, kp3_, 4'000LL, Crypto::hash_block(b0));
    st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::REPLACED);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::OUTSIDE_LINE);
}

TEST_F(RevocationTest, ForgedEndorsementByStolenAncestorIsVoid) {
    // Scheme В (blockchain.md §6.7 rules 10, 12): the parent names the owner's line, then
    // the parent's key is stolen and a forged record names a different one.
    Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    Block b1 = bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 2'000LL);
    KeyPair k1 = Crypto::generate_keypair();
    Block e = bc_->revoke_node(root_kp_.pub, 7, k1.pub, 3, kp3_, 3'000LL,
                               Crypto::hash_block(b1));

    KeyPair thief = Crypto::generate_keypair();
    RevocationPayload forged{};
    forged.revoked_node_index = 7;
    forged.revoked_pubkey     = kp7_.pub;
    forged.replacement_pubkey = thief.pub;
    forged.last_good          = LastGood{0, Crypto::hash_block(b0)};
    storage_->put_block(craft_revocation(3, kp3_, forged, 5'000LL));

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, thief.pub);                       // the forgery works — for now
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::OUTSIDE_LINE);

    // The grandparent revokes the parent and names its last valid block — E.
    bc_->revoke_node(root_kp_.pub, 3, std::nullopt, 1, kp1_, 6'000LL,
                     Crypto::hash_block(e));
    st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, k1.pub);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::CLEAN);
}

TEST_F(RevocationTest, ThreeTheftsInARow) {
    // Scheme «Три кражи подряд»: the branch key twice, then the parent's key.
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    Block b2 = bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 3'000LL);
    KeyPair k1 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, k1.pub, 3, kp3_, 3'500LL, Crypto::hash_block(b2));
    take(7, k1, 3'600LL);

    Block b3 = bc_->append_data_block(root_kp_.pub, 7, {0x03}, k1, 4'000LL);
    KeyPair k2 = Crypto::generate_keypair();
    Block r2 = bc_->revoke_node(root_kp_.pub, 7, k2.pub, 3, kp3_, 6'000LL,
                                Crypto::hash_block(b3));
    take(7, k2, 6'200LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x04}, k2, 6'500LL);

    // The parent's key is stolen: the thief takes the branch over with Kx.
    KeyPair kx = Crypto::generate_keypair();
    RevocationPayload hijack{};
    hijack.revoked_node_index = 7;
    hijack.revoked_pubkey     = k2.pub;
    hijack.replacement_pubkey = kx.pub;
    storage_->put_block(craft_revocation(3, kp3_, hijack, 8'000LL));
    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, kx.pub);

    // A higher key names the parent's last valid block — R2: the hijack is off its line.
    bc_->revoke_node(root_kp_.pub, 3, std::nullopt, 1, kp1_, 9'000LL,
                     Crypto::hash_block(r2));
    EXPECT_THROW(bc_->append_data_block(root_kp_.pub, 7, {0x05}, kx, 9'600LL),
                 RevocationError);
    bc_->append_data_block(root_kp_.pub, 7, {0x05}, k2, 9'600LL);

    st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::REPLACED);
    ASSERT_EQ(st.blocks.size(), 7u);   // two lines, two replacement blocks, K2's records
    for (auto s : st.blocks) EXPECT_EQ(s, BlockRevocationStatus::CLEAN);
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

// Потомков рождают по одному и в любом месте ветки родителя; второго можно позже
// (blockchain.md §11.2). Блок рождения подписан родителем и называет ключи потомка.
TEST_F(BlockchainTest, ChildrenAreBornOneByOneAnywhereInTheBranch) {
    std::map<NodeIndex, KeyPair> keys{{0, root_kp_}};
    const auto key_for = [&](NodeIndex i) {
        auto it = keys.find(i);
        if (it == keys.end()) it = keys.emplace(i, Crypto::generate_keypair()).first;
        return it->second;
    };
    bc_->ensure_path(root_kp_.pub, 1, key_for, 100LL);
    bc_->append_data_block(root_kp_.pub, 0, {0x80}, root_kp_, 200LL);
    bc_->ensure_path(root_kp_.pub, 2, key_for, 300LL);

    const Node n1 = storage_->get_node(root_kp_.pub, 1);
    const Node n2 = storage_->get_node(root_kp_.pub, 2);
    EXPECT_EQ(n1.birth.index, 0u);
    EXPECT_EQ(n2.birth.index, 2u);
    EXPECT_NO_THROW(validator_->validate_path(root_kp_.pub, 2));
    EXPECT_NO_THROW(validator_->validate_branch(root_kp_.pub, 0));

    // Ветка не старше рождения.
    bc_->append_data_block(root_kp_.pub, 2, {0x80}, keys.at(2), 299LL);
    EXPECT_THROW(validator_->validate_branch(root_kp_.pub, 2), TimestampError);
}

// ── The owner without a copy of the branch (blockchain.md §6.7 rule 12) ─────────

// Второе устройство того же хозяина — сейф с ключами предков: узлы и ветки предков
// есть, ветки 7 нет. Её блоки приходят извне (агрегатор, партнёр).
class RestoreTest : public RevocationTest {
protected:
    std::filesystem::path        safe_path_;
    std::unique_ptr<LmdbStorage> safe_;
    std::unique_ptr<Validator>   safe_validator_;
    std::unique_ptr<Blockchain>  safe_bc_;

    void SetUp() override {
        RevocationTest::SetUp();
        safe_path_ = db_path_.string() + "_safe";
        std::filesystem::remove_all(safe_path_);
        safe_           = std::make_unique<LmdbStorage>(safe_path_);
        safe_validator_ = std::make_unique<Validator>(*safe_);
        safe_bc_        = std::make_unique<Blockchain>(*safe_, *safe_validator_);
        for (NodeIndex i : {0u, 1u, 3u, 7u})
            safe_->put_node(root_kp_.pub, storage_->get_node(root_kp_.pub, i));
        for (NodeIndex i : {0u, 1u, 3u})
            for (const Block& b : bc_->get_branch(root_kp_.pub, i)) safe_->put_block(b);
    }
    void TearDown() override {
        safe_bc_.reset(); safe_validator_.reset(); safe_.reset();
        std::filesystem::remove_all(safe_path_);
        RevocationTest::TearDown();
    }
    static Block resigned(Block b, const KeyPair& kp) {
        b.signature      = Signature::null();
        const auto bytes = Serializer::encode(b);
        b.signature      = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        return b;
    }
};

// Линия называется по блокам, взятым извне, а замена продолжает сразу за ней.
TEST_F(RestoreTest, AnOwnerWithoutACopyNamesTheLineFromBlocksTakenElsewhere) {
    const Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    const Block b1 = bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 2'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x03}, kp7_, 3'000LL);   // вора, скажем

    // Без самих блоков сейф линию не назовёт.
    EXPECT_THROW(safe_bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 4'000LL,
                                       Crypto::hash_block(b1)),
                 InvalidArgumentError);

    safe_bc_->restore_branch(root_kp_.pub, 7, {b0, b1});
    EXPECT_EQ(safe_->branch_tip_index(root_kp_.pub, 7), std::optional<BlockIndex>(1u));

    const KeyPair repl = Crypto::generate_keypair();
    safe_bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 4'000LL, Crypto::hash_block(b1));
    const Block k = safe_bc_->take_replacement(root_kp_.pub, 7, repl, 4'500LL);
    EXPECT_EQ(k.address.block_index, 2u);
    EXPECT_EQ(k.prev_hash, Crypto::hash_block(b1));
    safe_bc_->append_data_block(root_kp_.pub, 7, {0x04}, repl, 5'000LL);

    const auto st = safe_validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 4u);
    for (auto s : st.blocks) EXPECT_EQ(s, BlockRevocationStatus::CLEAN);
    EXPECT_NO_THROW(safe_bc_->validate_branch(root_kp_.pub, 7));
}

// Стоп-кран сразу, линия потом: блоки под вопросом всё равно берутся, их вес
// решает названная линия, а не приём (правило 11).
TEST_F(RestoreTest, UnderAFullStopTheLineIsNamedLater) {
    const Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    safe_bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 2'000LL);
    safe_bc_->restore_branch(root_kp_.pub, 7, {b0});
    EXPECT_EQ(safe_validator_->branch_revocation_status(root_kp_.pub, 7).blocks.at(0),
              BlockRevocationStatus::SUSPECT);

    safe_bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, kp3_, 3'000LL, Crypto::hash_block(b0));
    EXPECT_EQ(safe_validator_->branch_revocation_status(root_kp_.pub, 7).blocks.at(0),
              BlockRevocationStatus::CLEAN);
}

// Своя копия кончается раньше: взятое продолжает её, и смена ключа внутри
// взятого участка переключает ожидаемый ключ (blockchain.md §6.6).
TEST_F(RestoreTest, TakenBlocksContinueTheCopyAndFollowAKeyChange) {
    const Block   b0  = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    const KeyPair k2  = Crypto::generate_keypair();
    const Block   rot = bc_->rotate_key(root_kp_.pub, 7, kp7_, k2, 2'000LL);
    const Block   b2  = bc_->append_data_block(root_kp_.pub, 7, {0x02}, k2, 3'000LL);
    safe_->put_block(b0);

    EXPECT_NO_THROW(safe_validator_->check_continuation(root_kp_.pub, 7, {rot, b2}));
    EXPECT_EQ(safe_->branch_tip_index(root_kp_.pub, 7), std::optional<BlockIndex>(0u));  // проверка не пишет

    safe_bc_->restore_branch(root_kp_.pub, 7, {rot, b2});
    EXPECT_EQ(safe_->branch_tip_index(root_kp_.pub, 7), std::optional<BlockIndex>(2u));
    EXPECT_NO_THROW(safe_bc_->validate_branch(root_kp_.pub, 7));
}

// Что не продолжает ветку, не берётся — и тогда не берётся ничего из пачки.
TEST_F(RestoreTest, WhatDoesNotContinueTheBranchIsNotTaken) {
    const Block b0 = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    const Block b1 = bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 2'000LL);
    const auto take = [&](const std::vector<Block>& blocks) {
        safe_bc_->restore_branch(root_kp_.pub, 7, blocks);
    };

    EXPECT_THROW(take({b1}), ChainIntegrityError);                         // пропуск №0
    EXPECT_THROW(take({b1, b0}), ChainIntegrityError);                     // не по порядку
    const Block other = bc_->append_data_block(root_kp_.pub, 3, {0x05}, kp3_, 1'500LL);
    EXPECT_THROW(take({other}), ChainIntegrityError);                      // чужая ветка

    const KeyPair stranger = Crypto::generate_keypair();
    EXPECT_THROW(take({resigned(b0, stranger)}), SignatureError);           // чужая подпись
    Block tampered = b1;
    tampered.payload = {0x09};                                              // подмена после подписи
    EXPECT_THROW(take({b0, tampered}), SignatureError);

    Block early = b1;
    early.timestamp_claimed = 500LL;
    EXPECT_THROW(take({b0, resigned(early, kp7_)}), TimestampError);        // раньше предыдущего
    Block before_birth = b0;
    before_birth.timestamp_claimed = -5LL;
    EXPECT_THROW(take({resigned(before_birth, kp7_)}), TimestampError);     // раньше рождения

    EXPECT_FALSE(safe_->branch_tip_index(root_kp_.pub, 7).has_value());
}

// ── The replacement block (blockchain.md §6.7 rule 9, решение участника 2026-11-02) ──

// Заменённый родитель: блок замены сразу за линией, затем рождение и отзыв новым
// ключом — путь и сертификат проходят у любого проверяющего.
TEST_F(RevocationTest, AReplacedParentTakesItsKeyAndGivesBirth) {
    const Block   line3 = storage_->get_block({root_kp_.pub, 3, 0});   // the birth of 7
    const KeyPair repl3 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 3, repl3.pub, 1, kp1_, 1'000LL, Crypto::hash_block(line3));
    const Block k = take(3, repl3, 1'500LL);
    EXPECT_EQ(k.type, BlockType::KEY_REPLACEMENT);
    EXPECT_EQ(k.address.block_index, 1u);
    EXPECT_EQ(k.prev_hash, Crypto::hash_block(line3));

    const KeyPair kp8 = Crypto::generate_keypair();
    std::map<NodeIndex, KeyPair> keys{{0, root_kp_}, {1, kp1_}, {3, repl3}, {8, kp8}};
    bc_->ensure_path(root_kp_.pub, 8, [&](NodeIndex i) { return keys.at(i); }, 2'000LL);
    EXPECT_NO_THROW(validator_->validate_path(root_kp_.pub, 8));
    const KeyPath kp = KeyPathCert::build(*storage_, root_kp_.pub, 8);
    EXPECT_EQ(KeyPathCert::verify(kp), root_kp_.pub);

    const Block r7 = bc_->revoke_node(root_kp_.pub, 7, std::nullopt, 3, repl3, 3'000LL);
    EXPECT_NO_THROW(RevocationCert::verify(RevocationCert::build(*storage_, r7.address)));
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 3));
}

// Линия сдвинулась после блока замены: он больше не стоит сразу за ней, и его
// эпоха вместе со всем после неё — вне линии; дальше эта копия не пишет.
TEST_F(RevocationTest, AReplacementBlockOffTheLineTakesItsEpochOffTheLine) {
    const Block   b0   = bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    const KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 2'000LL, Crypto::hash_block(b0));
    take(7, repl, 2'500LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, repl, 3'000LL);

    RevocationPayload moved{};                         // the same epoch, a line before #0
    moved.revoked_node_index = 7;
    moved.revoked_pubkey     = kp7_.pub;
    moved.replacement_pubkey = repl.pub;
    moved.last_good          = LastGood{EMPTY_BRANCH_INDEX, origin(7)};
    storage_->put_block(craft_revocation(3, kp3_, moved, 4'000LL));

    const auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_TRUE(st.continuation_off_line);
    ASSERT_EQ(st.blocks.size(), 3u);
    for (auto s : st.blocks) EXPECT_EQ(s, BlockRevocationStatus::OUTSIDE_LINE);
    EXPECT_THROW(bc_->append_data_block(root_kp_.pub, 7, {0x03}, repl, 5'000LL), RevocationError);
}

// Ключ замены без блока замены — не ключ ветки: такой блок ветку ломает.
TEST_F(RevocationTest, AReplacementKeyBeforeItsBlockIsNoKeyOfTheBranch) {
    const KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 1'000LL, origin(7));
    Block b{};
    b.address           = {root_kp_.pub, 7, 0};
    b.prev_hash         = origin(7);
    b.timestamp_claimed = 2'000LL;
    b.type              = BlockType::DATA;
    b.payload           = {0x80};
    b.signature         = Signature::null();
    auto bytes          = Serializer::encode(b);
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), repl.sec);
    storage_->put_block(b);
    EXPECT_THROW(validator_->branch_revocation_status(root_kp_.pub, 7), SignatureError);
}

// Блок замены, называющий не ту запись, ключа не меняет.
TEST_F(RevocationTest, AReplacementBlockNamingAnotherRecordDoesNotSwitch) {
    const KeyPair repl = Crypto::generate_keypair();
    const Block   rec  = bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 1'000LL, origin(7));
    Block k{};
    k.address           = {root_kp_.pub, 7, 0};
    k.prev_hash         = origin(7);
    k.timestamp_claimed = 2'000LL;
    k.type              = BlockType::KEY_REPLACEMENT;
    Hash wrong          = Crypto::hash_block(rec);
    wrong.bytes[0] ^= 0xFF;
    k.payload           = Serializer::encode(ReplacementPayload{rec.address, wrong});
    k.signature         = Signature::null();
    auto bytes          = Serializer::encode(k);
    k.signature         = Crypto::sign(bytes.data(), bytes.size(), repl.sec);
    storage_->put_block(k);
    EXPECT_THROW(validator_->branch_revocation_status(root_kp_.pub, 7), SignatureError);
}

// Блок замены пишет только ожидаемая замена.
TEST_F(RevocationTest, OnlyTheAwaitedReplacementWritesItsBlock) {
    EXPECT_THROW(take(7, kp7_, 1'000LL), RevocationError);            // nothing revoked
    const KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, repl.pub, 3, kp3_, 1'000LL, origin(7));
    EXPECT_THROW(take(7, Crypto::generate_keypair(), 2'000LL), RevocationError);
    take(7, repl, 2'000LL);
    EXPECT_THROW(take(7, repl, 3'000LL), RevocationError);            // taken already
}
