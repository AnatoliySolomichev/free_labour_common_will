#include "blockchain/key_path.h"
#include "blockchain/blockchain.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>
#include <filesystem>
#include <map>

using namespace blockchain;

// Attribution of a signing key to a chain (blockchain.md §7.5): the reader gets
// a node path and checks it alone — nobody is trusted for the answer.

class KeyPathTest : public ::testing::Test {
protected:
    std::filesystem::path        db_path_;
    std::unique_ptr<LmdbStorage> storage_;
    std::unique_ptr<Validator>   validator_;
    std::unique_ptr<Blockchain>  bc_;

    KeyPair                          root_kp_;
    UserId                           chain_{};
    std::map<NodeIndex, KeyPair>     keys_;

    static constexpr NodeIndex kLeaf = 0x7FFF'FFFFu;

    void SetUp() override {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_keypath_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_   = std::make_unique<LmdbStorage>(db_path_);
        validator_ = std::make_unique<Validator>(*storage_);
        bc_        = std::make_unique<Blockchain>(*storage_, *validator_);

        root_kp_ = Crypto::generate_keypair();
        chain_   = bc_->create_identity(root_kp_).structural_pubkey;
        keys_[0] = root_kp_;
        // ensure_path asks for the parent key too, so the source must be stable.
        bc_->ensure_path(chain_, kLeaf, [&](NodeIndex i) {
            auto it = keys_.find(i);
            if (it == keys_.end())
                it = keys_.emplace(i, Crypto::generate_keypair()).first;
            return it->second;
        });
    }

    void TearDown() override {
        bc_.reset(); validator_.reset(); storage_.reset();
        std::filesystem::remove_all(db_path_);
    }
};

TEST_F(KeyPathTest, BuiltPathNamesTheChainAndTheSigningKey) {
    const KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);

    EXPECT_EQ(kp.path.size(), 32u);           // root (depth 0) … leaf (depth 31)
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::signer_key(kp), keys_[kLeaf].pub);
}

TEST_F(KeyPathTest, SurvivesCborRoundTrip) {
    const KeyPath kp    = KeyPathCert::build(*storage_, chain_, kLeaf);
    const auto    bytes = Serializer::encode(kp);
    const KeyPath back  = Serializer::decode_key_path(bytes.data(), bytes.size());

    EXPECT_EQ(back.path, kp.path);
    EXPECT_EQ(KeyPathCert::verify(back), chain_);
}

TEST_F(KeyPathTest, PathOfAnInnerNodeIsShorterAndStillValid) {
    const NodeIndex inner = 7;  // depth 3, on the path to the leaf
    const KeyPath   kp    = KeyPathCert::build(*storage_, chain_, inner);

    EXPECT_EQ(kp.path.size(), 4u);
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::signer_key(kp), keys_[inner].pub);
}

// A forged path — a stranger's key grafted onto someone else's root — fails:
// the parent signature does not cover the substituted node.
TEST_F(KeyPathTest, GraftedKeyIsRejected) {
    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    kp.path.back().working_pubkey = Crypto::generate_keypair().pub;

    EXPECT_THROW(KeyPathCert::verify(kp), SignatureError);
}

// A whole path re-rooted at a different identity fails too: the root
// self-signature covers structural_pubkey.
TEST_F(KeyPathTest, ForeignRootIsRejected) {
    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    kp.path.front().structural_pubkey = Crypto::generate_keypair().pub;

    EXPECT_THROW(KeyPathCert::verify(kp), SignatureError);
}

TEST_F(KeyPathTest, BrokenLinkIsRejected) {
    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    kp.path.erase(kp.path.begin() + 5);

    EXPECT_THROW(KeyPathCert::verify(kp), ChainIntegrityError);
}

TEST_F(KeyPathTest, EmptyPathIsRejected) {
    EXPECT_THROW(KeyPathCert::verify(KeyPath{}), ChainIntegrityError);
}

// The reader finds the key among nodes it already holds — own and partners'
// (imported by merges, blockchain.md §6.4).
TEST_F(KeyPathTest, StorageScanAttributesAKnownKey) {
    const KeyPath kp     = KeyPathCert::build(*storage_, chain_, kLeaf);
    const auto    needle = KeyPathCert::signer_key(kp);

    UserId found{};
    bool   hit = false;
    storage_->for_each_node([&](const UserId& owner, const Node& n) {
        if (n.working_pubkey == needle) { found = owner; hit = true; return false; }
        return true;
    });

    EXPECT_TRUE(hit);
    EXPECT_EQ(found, chain_);
}

TEST_F(KeyPathTest, StorageScanDoesNotInventChains) {
    const auto stranger = Crypto::generate_keypair().pub;

    bool hit = false;
    storage_->for_each_node([&](const UserId&, const Node& n) {
        if (n.working_pubkey == stranger) { hit = true; return false; }
        return true;
    });

    EXPECT_FALSE(hit);
}
