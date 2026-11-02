#include "blockchain/key_path.h"
#include "blockchain/blockchain.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include "blockchain/revocation.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <map>

using namespace blockchain;

// Attribution of a signing key to a chain (blockchain.md §7.5): the reader gets
// a key path — the root's record, the births, every key change — and checks it
// alone; nobody is trusted for the answer.

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

    std::size_t changes_of(const KeyPath& kp, NodeIndex node) const {
        return static_cast<std::size_t>(std::count_if(kp.changes.begin(), kp.changes.end(),
            [&](const Block& c) { return c.address.node_index == node; }));
    }
    static Block resigned(Block b, const KeyPair& kp) {
        b.signature      = Signature::null();
        const auto bytes = Serializer::encode(b);
        b.signature      = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
        return b;
    }
    // A replacement block crafted by hand: right after `after` in `node`'s branch,
    // naming `record`, signed by `kp`.
    Block replacement_block(NodeIndex node, const Block& after, const Block& record,
                            const KeyPair& kp) {
        Block k{};
        k.address           = {chain_, node, after.address.block_index + 1};
        k.prev_hash         = Crypto::hash_block(after);
        k.timestamp_claimed = after.timestamp_claimed;
        k.type              = BlockType::KEY_REPLACEMENT;
        k.payload           = Serializer::encode(
            ReplacementPayload{record.address, Crypto::hash_block(record)});
        return resigned(k, kp);
    }
};

TEST_F(KeyPathTest, BuiltPathNamesTheChainAndTheSigningKey) {
    const KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);

    EXPECT_EQ(kp.births.size(), 31u);          // root (depth 0) … leaf (depth 31)
    EXPECT_EQ(KeyPathCert::nodes(kp).size(), 32u);
    EXPECT_EQ(KeyPathCert::node(kp), kLeaf);
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::signer_key(kp), keys_[kLeaf].pub);
}

TEST_F(KeyPathTest, SurvivesCborRoundTrip) {
    const KeyPath kp    = KeyPathCert::build(*storage_, chain_, kLeaf);
    const auto    bytes = Serializer::encode(kp);
    const KeyPath back  = Serializer::decode_key_path(bytes.data(), bytes.size());

    EXPECT_EQ(Serializer::encode(back), bytes);
    EXPECT_EQ(KeyPathCert::verify(back), chain_);
}

TEST_F(KeyPathTest, PathOfAnInnerNodeIsShorterAndStillValid) {
    const NodeIndex inner = 7;  // depth 3, on the path to the leaf
    const KeyPath   kp    = KeyPathCert::build(*storage_, chain_, inner);

    EXPECT_EQ(kp.births.size(), 3u);
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::signer_key(kp), keys_[inner].pub);
}

// A forged path — a stranger's key grafted onto someone else's root — fails:
// the parent's signature on the birth does not cover the substituted key.
TEST_F(KeyPathTest, GraftedKeyIsRejected) {
    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    const KeyPair stranger = Crypto::generate_keypair();
    kp.births.back().payload =
        Serializer::encode(BirthPayload{kLeaf, stranger.pub, stranger.pub});

    EXPECT_THROW(KeyPathCert::verify(kp), SignatureError);
}

// A whole path re-rooted at a different identity fails too: the root
// self-signature covers structural_pubkey.
TEST_F(KeyPathTest, ForeignRootIsRejected) {
    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    kp.root.structural_pubkey = Crypto::generate_keypair().pub;

    EXPECT_THROW(KeyPathCert::verify(kp), SignatureError);
}

TEST_F(KeyPathTest, BrokenLinkIsRejected) {
    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    kp.births.erase(kp.births.begin() + 5);

    EXPECT_THROW(KeyPathCert::verify(kp), ChainIntegrityError);
}

TEST_F(KeyPathTest, EmptyPathIsRejected) {
    EXPECT_THROW(KeyPathCert::verify(KeyPath{}), ChainIntegrityError);   // no root's record
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

// ── Смена рабочего ключа ветки (blockchain.md §6.6) ──────────────────────────
//
// До 2026-10-10 путь заканчивался первоначальным ключом узла, и подпись ветки,
// сменившей ключ, к цепи было не привести. Теперь к пути прикладываются звенья
// смены: каждое подписано ПРЕЖНИМ ключом и называет новый.

TEST_F(KeyPathTest, RotatedBranchIsStillAttributable) {
    const KeyPair fresh = Crypto::generate_keypair();
    bc_->rotate_key(chain_, kLeaf, keys_[kLeaf], fresh, 1'000);

    const KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    ASSERT_EQ(changes_of(kp, kLeaf), 1u);
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::signer_key(kp), fresh.pub);   // уже новый, не узловой
}

TEST_F(KeyPathTest, ChainOfRotationsWalksToTheNewestKey) {
    const KeyPair second = Crypto::generate_keypair();
    const KeyPair third  = Crypto::generate_keypair();
    bc_->rotate_key(chain_, kLeaf, keys_[kLeaf], second, 1'000);
    bc_->rotate_key(chain_, kLeaf, second,       third,  2'000);

    const KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    ASSERT_EQ(changes_of(kp, kLeaf), 2u);
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::signer_key(kp), third.pub);
}

TEST_F(KeyPathTest, RotationsSurviveCborRoundTrip) {
    const KeyPair fresh = Crypto::generate_keypair();
    bc_->rotate_key(chain_, kLeaf, keys_[kLeaf], fresh, 1'000);

    const KeyPath kp    = KeyPathCert::build(*storage_, chain_, kLeaf);
    const auto    bytes = Serializer::encode(kp);
    const KeyPath back  = Serializer::decode_key_path(bytes.data(), bytes.size());

    ASSERT_EQ(changes_of(back, kLeaf), 1u);
    EXPECT_EQ(KeyPathCert::verify(back), chain_);
    EXPECT_EQ(KeyPathCert::signer_key(back), fresh.pub);
}

// Подделка: звено смены, подписанное не прежним ключом, а чьим попало —
// так чужой ключ не привяжешь к этой цепи.
TEST_F(KeyPathTest, RotationSignedByAStrangerIsRejected) {
    const KeyPair fresh = Crypto::generate_keypair();
    bc_->rotate_key(chain_, kLeaf, keys_[kLeaf], fresh, 1'000);

    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    ASSERT_EQ(kp.changes.size(), 1u);
    kp.changes[0] = resigned(kp.changes[0], Crypto::generate_keypair());

    EXPECT_THROW(KeyPathCert::verify(kp), SignatureError);
}

// Звено смены ветки, которой нет на пути, к этому пути не относится.
TEST_F(KeyPathTest, RotationFromAnotherBranchIsRejected) {
    const KeyPair fresh = Crypto::generate_keypair();
    bc_->rotate_key(chain_, kLeaf, keys_[kLeaf], fresh, 1'000);

    KeyPath kp = KeyPathCert::build(*storage_, chain_, kLeaf);
    ASSERT_EQ(kp.changes.size(), 1u);
    kp.changes[0].address.node_index = 2;       // путь ведёт к листу через 1, не через 2

    EXPECT_THROW(KeyPathCert::verify(kp), ChainIntegrityError);
}

// ── История ключа родителя (blockchain.md §11.2, решение 2026-11-02) ──────────
//
// Подпись узла на месте i его ветки проверяется ключом, действующим там: ключ из
// рождения, проведённый через смены ключа до i.

// Родитель сменил ключ и рождает новым; потомок, рождённый до смены, стоит со
// старым. Вор со старым ключом после смены не родит.
TEST_F(KeyPathTest, ARotatedParentGivesBirth) {
    const KeyPair fresh = Crypto::generate_keypair();
    bc_->rotate_key(chain_, 1, keys_[1], fresh, 1'000);
    keys_[4] = Crypto::generate_keypair();
    bc_->ensure_path(chain_, 4, [&](NodeIndex i) { return i == 1 ? fresh : keys_.at(i); }, 2'000);

    const KeyPath kp = KeyPathCert::build(*storage_, chain_, 4);
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::key_at(kp, 1, 0), keys_[1].pub);      // до смены
    EXPECT_EQ(KeyPathCert::key_at(kp, 1, KeyPathCert::kNow), fresh.pub);
    EXPECT_NO_THROW(validator_->validate_path(chain_, 4));
    EXPECT_NO_THROW(validator_->validate_path(chain_, kLeaf));   // родившиеся раньше

    KeyPath forged = kp;   // рождение после смены, подписанное старым ключом
    forged.births.back() = resigned(forged.births.back(), keys_[1]);
    EXPECT_THROW(KeyPathCert::verify(forged), SignatureError);
}

// Ключ родителя заменён записью деда; блок замены стоит сразу за линией и называет
// запись. Заменённый родитель рождает и отзывает — путь и сертификат проходят.
TEST_F(KeyPathTest, AReplacedParentGivesBirthAndRevokes) {
    const Block   line = storage_->get_block({chain_, 3, 0});   // рождение узла 7
    const KeyPair repl = Crypto::generate_keypair();
    const Block   rec  = bc_->revoke_node(chain_, 3, repl.pub, 1, keys_[1], 1'000,
                                          Crypto::hash_block(line));
    storage_->put_block(replacement_block(3, line, rec, repl));
    keys_[8] = Crypto::generate_keypair();
    bc_->ensure_path(chain_, 8, [&](NodeIndex i) { return i == 3 ? repl : keys_.at(i); }, 2'000);

    const KeyPath kp = KeyPathCert::build(*storage_, chain_, 8);
    ASSERT_EQ(kp.records.size(), 1u);
    EXPECT_EQ(KeyPathCert::verify(kp), chain_);
    EXPECT_EQ(KeyPathCert::key_at(kp, 3, KeyPathCert::kNow), repl.pub);
    EXPECT_NO_THROW(validator_->validate_path(chain_, 8));

    // Отзыв ключа потомка 7 тем же заменённым родителем.
    const Block r7 = bc_->revoke_node(chain_, 7, std::nullopt, 3, repl, 3'000);
    const auto cert = RevocationCert::build(*storage_, r7.address);
    EXPECT_NO_THROW(RevocationCert::verify(cert));
}

// Подделки блока замены: чужая подпись, не то место, запись не приложена.
TEST_F(KeyPathTest, AForgedReplacementIsRejected) {
    const Block   line = storage_->get_block({chain_, 3, 0});
    const KeyPair repl = Crypto::generate_keypair();
    const Block   rec  = bc_->revoke_node(chain_, 3, repl.pub, 1, keys_[1], 1'000,
                                          Crypto::hash_block(line));
    storage_->put_block(replacement_block(3, line, rec, repl));
    const KeyPath good = KeyPathCert::build(*storage_, chain_, 3);
    ASSERT_EQ(KeyPathCert::verify(good), chain_);

    KeyPath thief = good;   // ключ вора вместо замены
    thief.changes.back() = resigned(thief.changes.back(), keys_[3]);
    EXPECT_THROW(KeyPathCert::verify(thief), SignatureError);

    KeyPath elsewhere = good;   // не сразу за линией
    elsewhere.changes.back().address.block_index = 5;
    elsewhere.changes.back() = resigned(elsewhere.changes.back(), repl);
    EXPECT_THROW(KeyPathCert::verify(elsewhere), ChainIntegrityError);

    KeyPath bare = good;   // без записи деда
    bare.records.clear();
    EXPECT_THROW(KeyPathCert::verify(bare), ChainIntegrityError);
}
