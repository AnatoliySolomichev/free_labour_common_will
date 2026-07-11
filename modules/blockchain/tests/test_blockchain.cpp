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
