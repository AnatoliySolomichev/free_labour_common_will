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
    EXPECT_EQ(n.parent_hash, Hash::zero());
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
    // Block 0 chains to the node's own hash.
    EXPECT_EQ(b0.prev_hash, Crypto::hash_node(storage_->get_node(root_kp_.pub, 5)));

    // The same node still parents deeper structure: 5 → 11 works alongside.
    bc_->ensure_path(root_kp_.pub, 11, key_for);
    bc_->append_data_block(root_kp_.pub, 11, {0xBB}, keys.at(11), 1'001LL);

    EXPECT_EQ(bc_->get_branch(root_kp_.pub, 5).size(), 1u);
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

TEST_F(BlockchainTest, BranchTipHashEmptyBranchEqualsNodeHash) {
    setup_leaf_path();
    Node leaf     = storage_->get_node(root_kp_.pub, LEAF);
    Hash expected = Crypto::hash_node(leaf);
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
    EXPECT_EQ(b0.prev_hash, Crypto::hash_node(leaf));
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

// ── revoke_node / validate_revocation / effective_revocation (§6.7) ──────────

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
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp3_, 2'000LL);

    EXPECT_EQ(b.address.node_index, 3u);
    EXPECT_EQ(b.type, BlockType::REVOCATION);

    auto rp = Serializer::decode_revocation_payload(b.payload.data(), b.payload.size());
    EXPECT_EQ(rp.revoked_node_index, 7u);
    EXPECT_EQ(rp.revoked_pubkey, kp7_.pub);
    EXPECT_EQ(rp.compromised_since, 1'000LL);
    EXPECT_FALSE(rp.replacement_pubkey.has_value());

    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 3));
    EXPECT_NO_THROW(validator_->validate_revocation(b));
}

TEST_F(RevocationTest, RevokeByGrandparent) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 1, kp1_, 2'000LL);
    EXPECT_EQ(b.address.node_index, 1u);
    EXPECT_NO_THROW(validator_->validate_revocation(b));
}

TEST_F(RevocationTest, NonAncestorAuthorThrows) {
    // Node 4 is in a sibling subtree of 3 (path 0 → 1 → 4): not an ancestor of 7.
    KeyPair kp4 = Crypto::generate_keypair();
    std::map<NodeIndex, KeyPair> keys{{0, root_kp_}, {1, kp1_}, {4, kp4}};
    bc_->ensure_path(root_kp_.pub, 4, [&](NodeIndex i) { return keys.at(i); });

    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 4, kp4, 2'000LL),
        RevocationError);
}

TEST_F(RevocationTest, SelfRevocationThrows) {
    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 7, kp7_, 2'000LL),
        RevocationError);
}

TEST_F(RevocationTest, RootRevocationThrows) {
    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 0, 1'000LL, std::nullopt, 0, root_kp_, 2'000LL),
        RevocationError);
}

TEST_F(RevocationTest, AncestorKeypairMismatchThrows) {
    // Signing with node 1's key while writing into node 3's branch.
    EXPECT_THROW(
        bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp1_, 2'000LL),
        InvalidArgumentError);
}

TEST_F(RevocationTest, StopThenReplaceLastWordWins) {
    // Night: emergency stop. Later: a second block assigns the replacement.
    bc_->revoke_node(root_kp_.pub, 7, 5'000LL, std::nullopt, 3, kp3_, 5'100LL);
    KeyPair fresh = Crypto::generate_keypair();
    Block second = bc_->revoke_node(root_kp_.pub, 7, 5'000LL, fresh.pub, 3, kp3_, 9'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->compromised_since, 5'000LL);
    ASSERT_TRUE(st->replacement_pubkey.has_value());
    EXPECT_EQ(*st->replacement_pubkey, fresh.pub);
    EXPECT_EQ(st->latest, second.address);

    EXPECT_NO_THROW(validator_->validate_revocation(second));
}

TEST_F(RevocationTest, EarliestCompromisedSinceIsKept) {
    // A later block cannot shrink retroactivity: min(compromised_since) wins.
    bc_->revoke_node(root_kp_.pub, 7, 3'000LL, std::nullopt, 3, kp3_, 5'000LL);
    KeyPair fresh = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 8'000LL, fresh.pub, 3, kp3_, 9'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->compromised_since, 3'000LL);
}

TEST_F(RevocationTest, HigherAncestorWins) {
    KeyPair repl_parent = Crypto::generate_keypair();
    KeyPair repl_grand  = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, repl_parent.pub, 3, kp3_, 2'000LL);
    bc_->revoke_node(root_kp_.pub, 7, 4'000LL, repl_grand.pub, 1, kp1_, 5'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    // Grandparent's branch decides, parent's replacement is ignored (§4.4).
    EXPECT_EQ(st->latest.node_index, 1u);
    ASSERT_TRUE(st->replacement_pubkey.has_value());
    EXPECT_EQ(*st->replacement_pubkey, repl_grand.pub);
    EXPECT_EQ(st->compromised_since, 4'000LL);
}

TEST_F(RevocationTest, EffectiveRevocationNoneIsNullopt) {
    EXPECT_FALSE(validator_->effective_revocation(root_kp_.pub, 7).has_value());
}

TEST_F(RevocationTest, WrongRevokedPubkeyRejected) {
    RevocationPayload rp{};
    rp.revoked_node_index = 7;
    rp.revoked_pubkey     = Crypto::generate_keypair().pub; // never was node 7's key
    rp.compromised_since  = 1'000LL;

    Block b = craft_revocation(3, kp3_, rp, 2'000LL);
    EXPECT_THROW(validator_->validate_revocation(b), RevocationError);
}

TEST_F(RevocationTest, RevokeAfterRotationUsesCurrentKey) {
    KeyPair rotated = Crypto::generate_keypair();
    bc_->rotate_key(root_kp_.pub, 7, kp7_, rotated, 1'000LL);

    Block b = bc_->revoke_node(root_kp_.pub, 7, 2'000LL, std::nullopt, 3, kp3_, 3'000LL);
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
    rp.compromised_since  = 500LL;

    Block b = craft_revocation(3, kp3_, rp, 2'000LL);
    EXPECT_NO_THROW(validator_->validate_revocation(b));
}

TEST_F(RevocationTest, ReplacementKeyIsRevocableAgain) {
    // stop → replace → the replacement key itself gets compromised.
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, repl.pub, 3, kp3_, 2'000LL);

    RevocationPayload rp{};
    rp.revoked_node_index = 7;
    rp.revoked_pubkey     = repl.pub; // known only via the earlier replacement
    rp.compromised_since  = 3'000LL;

    Block b = craft_revocation(3, kp3_, rp, 4'000LL);
    storage_->put_block(b);
    EXPECT_NO_THROW(validator_->validate_revocation(b));

    // The last word: no new replacement → branch is frozen again.
    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    EXPECT_FALSE(st->replacement_pubkey.has_value());
}

// ── Step 2: revocation effects in validation (§6.7 rules 9–11) ────────────────

TEST_F(RevocationTest, StatusCleanSuspectSplit) {
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, kp7_, 2'000LL);
    bc_->revoke_node(root_kp_.pub, 7, 1'500LL, std::nullopt, 3, kp3_, 3'000LL);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::FROZEN);
    EXPECT_FALSE(st.next_key.has_value());
    ASSERT_EQ(st.blocks.size(), 2u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);   // ts 1000 ≤ since 1500
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::SUSPECT); // ts 2000 > since 1500
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
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp3_, 2'000LL);
    EXPECT_THROW(
        bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 3'000LL),
        RevocationError);
}

TEST_F(RevocationTest, ReplacedBranchAcceptsOnlyReplacement) {
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, repl.pub, 3, kp3_, 2'000LL);

    EXPECT_THROW(
        bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 3'000LL),
        RevocationError);

    Block b = bc_->append_data_block(root_kp_.pub, 7, {0x01}, repl, 3'000LL);
    EXPECT_EQ(b.address.block_index, 0u);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    EXPECT_EQ(st.state, BranchRevocationState::REPLACED);
    ASSERT_EQ(st.blocks.size(), 1u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::CLEAN);
    ASSERT_TRUE(st.next_key.has_value());
    EXPECT_EQ(*st.next_key, repl.pub);

    // validate_branch follows the out-of-branch key switch (§6.7 rule 9).
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

TEST_F(RevocationTest, SwitchAfterExistingBlocks) {
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 500LL, repl.pub, 3, kp3_, 2'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x02}, repl, 3'000LL);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 2u);
    EXPECT_EQ(st.blocks[0], BlockRevocationStatus::SUSPECT); // ts 1000 > since 500
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::CLEAN);   // signed by replacement
    EXPECT_NO_THROW(bc_->validate_branch(root_kp_.pub, 7));
}

TEST_F(RevocationTest, OldKeyAfterSwitchIsInvalid) {
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, repl.pub, 3, kp3_, 2'000LL);
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, repl, 3'000LL);

    // The thief appends with the old key after the switch (bypassing the facade).
    Hash prev = bc_->branch_tip_hash(root_kp_.pub, 7);
    Block b{};
    b.address           = {root_kp_.pub, 7, 1};
    b.prev_hash         = prev;
    b.timestamp_claimed = 4'000LL;
    b.type              = BlockType::DATA;
    b.payload           = {0x80};
    b.signature         = Signature::null();
    auto bytes          = Serializer::encode(b);
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp7_.sec);
    storage_->put_block(b);

    auto st = validator_->branch_revocation_status(root_kp_.pub, 7);
    ASSERT_EQ(st.blocks.size(), 2u);
    EXPECT_EQ(st.blocks[1], BlockRevocationStatus::INVALID_AFTER_REPLACEMENT);
    EXPECT_THROW(bc_->validate_branch(root_kp_.pub, 7), SignatureError);
}

TEST_F(RevocationTest, RotationAfterReplacementContinues) {
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, repl.pub, 3, kp3_, 2'000LL);
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
    bc_->revoke_node(root_kp_.pub, 7, 8'000LL, thief_repl.pub, 3, kp3_, 9'000LL);

    // The owner escalates: node 1 revokes node 3 as compromised since 5'000 —
    // earlier than the thief's record (ts 9'000), so that record loses weight
    // (§6.7 rule 10).
    bc_->revoke_node(root_kp_.pub, 3, 5'000LL, std::nullopt, 1, kp1_, 10'000LL);

    EXPECT_FALSE(validator_->effective_revocation(root_kp_.pub, 7).has_value());
}

TEST_F(RevocationTest, PreCompromiseAuthorRecordStillCounts) {
    // A record written before the author's own compromise moment keeps weight.
    bc_->revoke_node(root_kp_.pub, 7, 2'000LL, std::nullopt, 3, kp3_, 3'000LL);
    bc_->revoke_node(root_kp_.pub, 3, 5'000LL, std::nullopt, 1, kp1_, 10'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->compromised_since, 2'000LL);
}

TEST_F(RevocationTest, ReplacedAuthorRevokesWithNewKey) {
    // Node 3 was revoked and replaced; the owner keeps administering 7 from 3 —
    // records signed by the replacement count (§6.7 rule 10).
    KeyPair repl3 = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 3, 5'000LL, repl3.pub, 1, kp1_, 6'000LL);
    bc_->revoke_node(root_kp_.pub, 7, 7'000LL, std::nullopt, 3, repl3, 8'000LL);

    auto st = validator_->effective_revocation(root_kp_.pub, 7);
    ASSERT_TRUE(st.has_value());
    EXPECT_EQ(st->compromised_since, 7'000LL);
}

TEST_F(RevocationTest, NodeInvalidatedByRevocation) {
    // Children of 7: 15 (created after the compromise moment) and 16 (before).
    auto make_node = [&](NodeIndex idx, Timestamp created_at,
                         const Node& parent, const KeyPair& parent_kp) {
        KeyPair kp = Crypto::generate_keypair();
        Node n{};
        n.index             = idx;
        n.structural_pubkey = kp.pub;
        n.working_pubkey    = kp.pub;
        n.parent_hash       = Crypto::hash_node(parent);
        n.created_at        = created_at;
        n.parent_sig        = Signature::null();
        auto bytes          = Serializer::encode(n);
        n.parent_sig        = Crypto::sign(bytes.data(), bytes.size(), parent_kp.sec);
        storage_->put_node(root_kp_.pub, n);
        return kp;
    };
    Node node7 = bc_->get_node(root_kp_.pub, 7);
    KeyPair kp15 = make_node(15, 9'000LL, node7, kp7_);
    make_node(16, 1'000LL, node7, kp7_);
    // Grandchild under the poisoned edge: "created before" — still invalid.
    Node node15 = bc_->get_node(root_kp_.pub, 15);
    make_node(31, 100LL, node15, kp15);

    bc_->revoke_node(root_kp_.pub, 7, 5'000LL, std::nullopt, 3, kp3_, 6'000LL);

    EXPECT_TRUE (validator_->node_invalidated_by_revocation(root_kp_.pub, 15));
    EXPECT_FALSE(validator_->node_invalidated_by_revocation(root_kp_.pub, 16));
    EXPECT_TRUE (validator_->node_invalidated_by_revocation(root_kp_.pub, 31));
    EXPECT_FALSE(validator_->node_invalidated_by_revocation(root_kp_.pub, 7));
}

TEST_F(RevocationTest, StorageIndexReturnsAddresses) {
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp3_, 2'000LL);
    KeyPair repl = Crypto::generate_keypair();
    bc_->revoke_node(root_kp_.pub, 7, 1'000LL, repl.pub, 1, kp1_, 3'000LL);

    auto addrs = storage_->get_revocation_addresses(root_kp_.pub, 7);
    ASSERT_EQ(addrs.size(), 2u);
    EXPECT_EQ(addrs[0].node_index, 3u);
    EXPECT_EQ(addrs[1].node_index, 1u);
    EXPECT_TRUE(storage_->get_revocation_addresses(root_kp_.pub, 15).empty());
}

// ── Step 3: self-verifying revocation certificate (§6.7 rule 8) ───────────────

TEST_F(RevocationTest, CertificateBuildAndVerify) {
    KeyPair repl = Crypto::generate_keypair();
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, repl.pub, 3, kp3_, 2'000LL);

    auto cert = RevocationCert::build(*storage_, b.address);
    EXPECT_EQ(cert.path.size(), 3u); // nodes 0 → 1 → 3
    EXPECT_NO_THROW(RevocationCert::verify(cert));

    auto rp = RevocationCert::payload(cert);
    EXPECT_EQ(rp.revoked_node_index, 7u);
    EXPECT_EQ(rp.revoked_pubkey, kp7_.pub);
    ASSERT_TRUE(rp.replacement_pubkey.has_value());
    EXPECT_EQ(*rp.replacement_pubkey, repl.pub);
}

TEST_F(RevocationTest, CertificateSurvivesSerialization) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 1, kp1_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);

    auto enc = Serializer::encode(cert);
    auto dec = Serializer::decode_revocation_cert(enc.data(), enc.size());

    EXPECT_NO_THROW(RevocationCert::verify(dec));
    EXPECT_EQ(dec.path.size(), cert.path.size()); // grandparent author: 0 → 1
    EXPECT_EQ(Crypto::hash_block(dec.block), Crypto::hash_block(cert.block));
}

TEST_F(RevocationTest, CertificateTamperedNodeRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp3_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);
    cert.path[1].created_at += 1; // breaks node 1's own parent signature
    EXPECT_THROW(RevocationCert::verify(cert), SignatureError);
}

TEST_F(RevocationTest, CertificateTruncatedPathRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp3_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);
    cert.path.pop_back(); // tip no longer matches the block's node
    EXPECT_THROW(RevocationCert::verify(cert), ChainIntegrityError);
}

TEST_F(RevocationTest, CertificateTamperedPayloadRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp3_, 2'000LL);
    auto cert = RevocationCert::build(*storage_, b.address);
    cert.block.payload.push_back(0x00); // breaks the block signature
    EXPECT_THROW(RevocationCert::verify(cert), SignatureError);
}

TEST_F(RevocationTest, CertificateForeignRootRejected) {
    Block b = bc_->revoke_node(root_kp_.pub, 7, 1'000LL, std::nullopt, 3, kp3_, 2'000LL);
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
    rp.compromised_since  = 1'000LL;
    Block b = craft_revocation(4, kp4, rp, 2'000LL);

    RevocationCertificate cert{};
    cert.block = b;
    for (NodeIndex idx : path_indices(4))
        cert.path.push_back(storage_->get_node(root_kp_.pub, idx));
    EXPECT_THROW(RevocationCert::verify(cert), RevocationError);
}

// ── Step 4: imported foreign revocations feed the local index (§6.7 rule 8) ───

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
        Block b = f_bc.revoke_node(f_root.pub, 7, 4'000LL, repl.pub, 3, f_kp3, 5'000LL);
        auto cert = RevocationCert::build(f_storage, b.address);

        // ── the "wire": only certificate bytes cross storages ──
        auto bytes = Serializer::encode(cert);
        auto recv  = Serializer::decode_revocation_cert(bytes.data(), bytes.size());
        RevocationCert::verify(recv);

        // Import into OUR storage (as bc revoke fetch does).
        const UserId chain = recv.block.address.user_id;
        for (const auto& node : recv.path)
            if (!storage_->has_node(chain, node.index))
                storage_->put_node(chain, node);
        storage_->put_external_block(recv.block);

        // The partner's node 7 is now revoked in our view.
        auto st = validator_->effective_revocation(chain, 7);
        ASSERT_TRUE(st.has_value());
        EXPECT_EQ(st->compromised_since, 4'000LL);
        ASSERT_TRUE(st->replacement_pubkey.has_value());
        EXPECT_EQ(*st->replacement_pubkey, repl.pub);
    }
    std::filesystem::remove_all(foreign_db);
}

// ── Step 5: acceptance-time revocation checks (§6.7 rule 11) ──────────────────

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
    for (const auto& node : recv.path)
        if (!into.has_node(f.uid(), node.index))
            into.put_node(f.uid(), node);
    into.put_external_block(recv.block);
}

TEST_F(RevocationTest, TipCheckRefusesFrozenPartnerBranch) {
    ForeignChain f("frozen");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    Block rev = f.bc->revoke_node(f.uid(), 7, 500LL, std::nullopt, 3, f.kp3, 2'000LL);
    import_certificate(*storage_, f, rev);

    EXPECT_THROW(validator_->check_tip_against_revocations(f.tip(7)),
                 RevocationError);
}

TEST_F(RevocationTest, TipCheckRefusesStaleTipOfReplacedBranch) {
    ForeignChain f("stale");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    KeyPair repl = Crypto::generate_keypair();
    Block rev = f.bc->revoke_node(f.uid(), 7, 500LL, repl.pub, 3, f.kp3, 2'000LL);
    import_certificate(*storage_, f, rev);

    // The tip is still under the old key — stale or stolen.
    EXPECT_THROW(validator_->check_tip_against_revocations(f.tip(7)),
                 RevocationError);
}

TEST_F(RevocationTest, TipCheckAcceptsReplacedTip) {
    ForeignChain f("replaced");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    KeyPair repl = Crypto::generate_keypair();
    Block rev = f.bc->revoke_node(f.uid(), 7, 500LL, repl.pub, 3, f.kp3, 2'000LL);
    f.bc->append_data_block(f.uid(), 7, {0x02}, repl, 3'000LL); // re-tip under R
    import_certificate(*storage_, f, rev);

    EXPECT_NO_THROW(validator_->check_tip_against_revocations(f.tip(7)));
}

TEST_F(RevocationTest, TipCheckSilentWithoutLocalKnowledge) {
    ForeignChain f("unknown");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    f.bc->revoke_node(f.uid(), 7, 500LL, std::nullopt, 3, f.kp3, 2'000LL);
    // Certificate NOT imported: no local knowledge — no objection.
    // Keeping knowledge fresh is the caller's policy (sync.md §10.3).
    EXPECT_NO_THROW(validator_->check_tip_against_revocations(f.tip(7)));
}

TEST_F(RevocationTest, TipCheckRefusesFakeSubtreeNode) {
    ForeignChain f("fake");
    // Node 15 under 7, "created" after the compromise moment (§6.7 rule 6).
    KeyPair kp15 = Crypto::generate_keypair();
    Node n7 = f.bc->get_node(f.uid(), 7);
    Node n{};
    n.index             = 15;
    n.structural_pubkey = kp15.pub;
    n.working_pubkey    = kp15.pub;
    n.parent_hash       = Crypto::hash_node(n7);
    n.created_at        = 9'000LL;
    n.parent_sig        = Signature::null();
    auto nb = Serializer::encode(n);
    n.parent_sig = Crypto::sign(nb.data(), nb.size(), f.kp7.sec);
    f.storage->put_node(f.uid(), n);
    f.bc->append_data_block(f.uid(), 15, {0x01}, kp15, 9'500LL);

    Block rev = f.bc->revoke_node(f.uid(), 7, 5'000LL, std::nullopt, 3, f.kp3, 9'600LL);
    import_certificate(*storage_, f, rev);

    // Branch 15 itself is not revoked; the poisoned edge 7→15 is refused.
    EXPECT_THROW(validator_->check_tip_against_revocations(f.tip(15)),
                 RevocationError);
}

TEST_F(RevocationTest, VerifyPartnerTipChecksRevocations) {
    ForeignChain f("verify");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    Block rev = f.bc->revoke_node(f.uid(), 7, 500LL, std::nullopt, 3, f.kp3, 2'000LL);
    import_certificate(*storage_, f, rev);

    MergeSession session(*storage_, *validator_);
    EXPECT_THROW(session.verify_partner_tip(f.tip(7)), RevocationError);
}

TEST_F(RevocationTest, MergeFromRevokedOwnBranchRefused) {
    // A frozen own branch may not open new bilateral acts (merges bypass the
    // facade's append protection).
    bc_->append_data_block(root_kp_.pub, 7, {0x01}, kp7_, 1'000LL);
    bc_->revoke_node(root_kp_.pub, 7, 500LL, std::nullopt, 3, kp3_, 2'000LL);

    ForeignChain f("mergepeer");
    f.bc->append_data_block(f.uid(), 7, {0x01}, f.kp7, 1'000LL);
    MergeSession f_session(*f.storage, *f.validator);
    auto partner_tip  = f.tip(7);
    auto partner_snap = f_session.snapshot_for(f.uid(), 7);

    MergeSession session(*storage_, *validator_);
    EXPECT_THROW(
        session.create_pending(root_kp_.pub, 7, partner_tip, partner_snap,
                               kp7_, 3'000LL, 1),
        RevocationError);
}
