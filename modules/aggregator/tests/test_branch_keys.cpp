// Whose blocks the aggregator takes (sync.md §10.10): only those signed by
// their own branch — the node's working key, carried through the branch's key
// changes, or a replacement a revocation named. The branch's keys come from its
// key path, checked on arrival.

#include "aggregator/branch_keys.h"
#include "aggregator/server.h"

#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/serializer.h>
#include <blockchain/storage.h>
#include <blockchain/validator.h>

#include <gtest/gtest.h>
#include <httplib.h>

#include <filesystem>
#include <map>
#include <thread>

using namespace aggregator;
using namespace blockchain;

namespace {

constexpr NodeIndex kLeaf = 0x7FFF'FFFFu;

// A chain of its own, as a client keeps it: nodes root → leaf, one branch.
struct Chain {
    std::filesystem::path        dir;
    std::unique_ptr<LmdbStorage> storage;
    std::unique_ptr<Validator>   validator;
    std::unique_ptr<Blockchain>  bc;
    std::map<NodeIndex, KeyPair> keys;
    UserId                       uid{};
    Timestamp                    now = 1'800'000'000;

    explicit Chain(const std::string& name) {
        dir = std::filesystem::temp_directory_path() / ("bc_branch_keys_" + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        storage   = std::make_unique<LmdbStorage>(dir / "db");
        validator = std::make_unique<Validator>(*storage);
        bc        = std::make_unique<Blockchain>(*storage, *validator);
        for (NodeIndex idx : path_indices(kLeaf)) keys[idx] = Crypto::generate_keypair();
        uid = keys.at(0).pub;
        bc->create_identity(keys.at(0));
        bc->ensure_path(uid, kLeaf, [&](NodeIndex idx) { return keys.at(idx); });
    }
    ~Chain() {
        bc.reset();
        validator.reset();
        storage.reset();
        std::filesystem::remove_all(dir);
    }

    Block write(const KeyPair& kp) {
        return bc->append_data_block(uid, kLeaf, {0x80}, kp, ++now);
    }
    KeyPath path() const { return KeyPathCert::build(*storage, uid, kLeaf); }
};

// The block signed again by `kp`.
Block resigned(Block b, const KeyPair& kp) {
    b.signature = Signature::null();
    const auto bytes = Serializer::encode(b);
    b.signature = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

// The same block, signed by someone else.
Block forged(Block b) {
    const KeyPair stranger = Crypto::generate_keypair();
    b.signature = Signature::null();
    const auto bytes = Serializer::encode(b);
    b.signature = Crypto::sign(bytes.data(), bytes.size(), stranger.sec);
    return b;
}

std::string cbor(const Block& b) {
    const auto bytes = Serializer::encode(b);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}
std::string cbor(const KeyPath& kp) {
    const auto bytes = Serializer::encode(kp);
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::string hex(const std::array<uint8_t, 32>& a) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const auto byte : a) {
        out += digits[byte >> 4];
        out += digits[byte & 15];
    }
    return out;
}

// A live aggregator on `port` over `storage`, peering with `peers`.
struct Live {
    AggregatorServer server;
    std::thread      thread;
    httplib::Client  cli;
    Live(AggregatorStorage& storage, uint16_t port, std::vector<std::string> peers = {})
        : server(storage, port, std::move(peers), std::chrono::seconds(1)),
          thread([this] { server.run(); }),
          cli("127.0.0.1:" + std::to_string(port)) {
        for (int i = 0; i < 100; ++i) {
            if (auto res = cli.Get("/stats"); res && res->status == 200) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    ~Live() {
        server.stop();
        thread.join();
    }
    int status(httplib::Result res) { return res ? res->status : -1; }
};

} // namespace

// Без пути ветка неизвестна; с проверенным путём — её блоки свои, а подписанное
// чужим ключом — нет.
TEST(BranchKeys, ABranchIsKnownByItsPath) {
    Chain       a("path");
    const Block b = a.write(a.keys.at(kLeaf));
    BranchKeys  keys;
    EXPECT_EQ(keys.check(b), BranchKeys::Verdict::unknown_branch);
    EXPECT_EQ(keys.learn_path(a.path()), a.uid);
    EXPECT_EQ(keys.check(b), BranchKeys::Verdict::signed_by_branch);
    EXPECT_EQ(keys.check(forged(b)), BranchKeys::Verdict::foreign_signature);
    ASSERT_TRUE(keys.path(a.uid, kLeaf).has_value());
    EXPECT_FALSE(keys.path(a.uid, 0).has_value());
}

// Путь, который не сходится, не учит ничему: звено подписано не родителем.
TEST(BranchKeys, APathThatDoesNotCheckIsRefused) {
    Chain   a("bad_path");
    KeyPath kp = a.path();
    kp.path.back().working_pubkey = Crypto::generate_keypair().pub;
    BranchKeys keys;
    EXPECT_THROW(keys.learn_path(kp), std::exception);
    EXPECT_EQ(keys.check(a.write(a.keys.at(kLeaf))), BranchKeys::Verdict::unknown_branch);
}

// Смена ключа (blockchain.md §6.6): блоки после неё подписаны новым ключом и
// принимаются, когда пришло само звено смены — или путь, который его несёт.
TEST(BranchKeys, AKeyChangeCarriesTheBranch) {
    Chain         a("rotation");
    BranchKeys    keys;
    keys.learn_path(a.path());
    const KeyPair fresh   = Crypto::generate_keypair();
    const Block   rotated = a.bc->rotate_key(a.uid, kLeaf, a.keys.at(kLeaf), fresh, ++a.now);
    const Block   after   = a.write(fresh);
    EXPECT_EQ(keys.check(rotated), BranchKeys::Verdict::signed_by_branch);   // прежним ключом
    EXPECT_EQ(keys.check(after), BranchKeys::Verdict::foreign_signature);
    keys.learn_rotation(rotated);
    EXPECT_EQ(keys.check(after), BranchKeys::Verdict::signed_by_branch);
    EXPECT_EQ(keys.path(a.uid, kLeaf)->rotations.size(), 1u);

    BranchKeys fresh_keys;                                   // путь со сменой — то же самое
    fresh_keys.learn_path(a.path());
    EXPECT_EQ(fresh_keys.check(after), BranchKeys::Verdict::signed_by_branch);
}

// Замена из отзыва (blockchain.md §6.7): предок, ключ которого выше, решает за
// ветку — после момента компрометации подписывает только названная им замена,
// а написанное старым ключом до того остаётся.
TEST(BranchKeys, AfterTheCompromiseOnlyTheReplacementSigns) {
    Chain           a("revocation");
    BranchKeys      keys;
    keys.learn_path(a.path());
    const KeyPair&  old         = a.keys.at(kLeaf);
    const Block     before      = a.write(old);
    const KeyPair   replacement = Crypto::generate_keypair();
    const NodeIndex parent      = (kLeaf - 1) / 2;
    const Block     rev = a.bc->revoke_node(a.uid, kLeaf, a.now, replacement.pub, parent,
                                            a.keys.at(parent), ++a.now);
    const Block after  = a.write(replacement);
    const Block stolen = resigned(after, old);               // старым ключом после компрометации
                                                             // (своя цепь такого и не напишет)
    EXPECT_EQ(keys.check(after), BranchKeys::Verdict::foreign_signature);
    EXPECT_EQ(keys.check(stolen), BranchKeys::Verdict::signed_by_branch);   // отзыва ещё не слышно
    keys.learn_revocation(RevocationCert::build(*a.storage, rev.address));
    EXPECT_EQ(keys.check(after), BranchKeys::Verdict::signed_by_branch);
    EXPECT_EQ(keys.check(stolen), BranchKeys::Verdict::foreign_signature);
    EXPECT_EQ(keys.check(before), BranchKeys::Verdict::signed_by_branch);
}

// Из двух отзывов одной ветки решает тот, чей автор ближе к корню
// (blockchain.md §4.4) — в каком бы порядке они ни пришли.
TEST(BranchKeys, TheHigherAncestorDecides) {
    Chain           a("two_revocations");
    const NodeIndex parent = (kLeaf - 1) / 2;
    const NodeIndex grand  = (parent - 1) / 2;
    const KeyPair   low    = Crypto::generate_keypair();
    const KeyPair   high   = Crypto::generate_keypair();
    const Block rev_low  = a.bc->revoke_node(a.uid, kLeaf, a.now, low.pub, parent,
                                             a.keys.at(parent), ++a.now);
    const Block rev_high = a.bc->revoke_node(a.uid, kLeaf, a.now, high.pub, grand,
                                             a.keys.at(grand), ++a.now);
    const Block by_high = a.write(high);
    const Block by_low  = resigned(by_high, low);            // своя цепь его уже не напишет
    for (const bool high_first : {false, true}) {
        BranchKeys keys;
        keys.learn_path(a.path());
        const auto cert_low  = RevocationCert::build(*a.storage, rev_low.address);
        const auto cert_high = RevocationCert::build(*a.storage, rev_high.address);
        keys.learn_revocation(high_first ? cert_high : cert_low);
        keys.learn_revocation(high_first ? cert_low : cert_high);
        EXPECT_EQ(keys.check(by_high), BranchKeys::Verdict::signed_by_branch);
        EXPECT_EQ(keys.check(by_low), BranchKeys::Verdict::foreign_signature);
    }
}

// Число смен ключа ничего не решает: линия смен, что расходится с известной, —
// развилка ветки того же ранга, сколько бы звеньев в ней ни было; её ключи не
// подписывают, пока предок не рассудит отзывом. Продолжение известной линии —
// новость, его берут.
TEST(BranchKeys, ALongerDivergingLineDoesNotWin) {
    Chain         a("fork");
    const KeyPair k1 = Crypto::generate_keypair();
    a.bc->rotate_key(a.uid, kLeaf, a.keys.at(kLeaf), k1, ++a.now);
    BranchKeys keys;
    keys.learn_path(a.path());                               // известная линия: K0 → K1
    // Чужая линия от того же K0: K0 → K2 → K3 — на звено длиннее.
    const KeyPath known = a.path();
    const KeyPair k2 = Crypto::generate_keypair();
    const KeyPair k3 = Crypto::generate_keypair();
    Block r1 = known.rotations.front();
    std::copy(k2.pub.bytes.begin(), k2.pub.bytes.end(), r1.payload.begin());
    r1 = resigned(r1, a.keys.at(kLeaf));
    Block r2 = r1;
    r2.address.block_index += 1;
    r2.prev_hash = Crypto::hash_block(r1);
    std::copy(k3.pub.bytes.begin(), k3.pub.bytes.end(), r2.payload.begin());
    r2 = resigned(r2, k2);
    KeyPath diverging = known;
    diverging.rotations = {r1, r2};
    ASSERT_EQ(KeyPathCert::verify(diverging), a.uid);       // сама по себе она сходится
    keys.learn_path(diverging);
    EXPECT_EQ(keys.check(a.write(k3)), BranchKeys::Verdict::foreign_signature);
    EXPECT_EQ(keys.check(a.write(k1)), BranchKeys::Verdict::signed_by_branch);
    EXPECT_EQ(keys.path(a.uid, kLeaf)->rotations.size(), 1u);

    const KeyPair k4 = Crypto::generate_keypair();           // продолжение известной — берут
    a.bc->rotate_key(a.uid, kLeaf, k1, k4, ++a.now);
    keys.learn_path(a.path());
    EXPECT_EQ(keys.check(a.write(k4)), BranchKeys::Verdict::signed_by_branch);
}

// Вживую (sync.md §10.10): блок неизвестной ветки — «дайте путь» (428), путь
// проверяется на входе, после него блок принят, подделка — нет (403), а путь
// ветки читатель берёт по её адресу.
TEST(BranchKeys, TheAggregatorTakesOnlyBlocksSignedByTheirBranch) {
    Chain a("server");
    const auto db = std::filesystem::temp_directory_path() / "bc_branch_keys_server_db";
    std::filesystem::remove_all(db);
    const Block b = a.write(a.keys.at(kLeaf));
    {
        AggregatorStorage storage(db);
        {
            Live          agg(storage, 18960);
            const KeyPath kp      = a.path();
            const auto    key_hex = hex(KeyPathCert::signer_key(kp).bytes);
            EXPECT_EQ(agg.status(agg.cli.Post("/blocks", cbor(b), "application/cbor")), 428);
            EXPECT_EQ(agg.status(agg.cli.Post("/keypaths/" + std::string(64, 'a'), cbor(kp),
                                              "application/cbor")),
                      400);                                  // путь ведёт к другому ключу
            EXPECT_EQ(agg.status(agg.cli.Post("/keypaths/" + key_hex, "garbage",
                                              "application/cbor")),
                      400);
            EXPECT_EQ(agg.status(agg.cli.Post("/keypaths/" + key_hex, cbor(kp),
                                              "application/cbor")),
                      200);
            EXPECT_EQ(agg.status(agg.cli.Post("/blocks", cbor(b), "application/cbor")), 200);
            EXPECT_EQ(agg.status(agg.cli.Post("/blocks", cbor(forged(a.write(a.keys.at(kLeaf)))),
                                              "application/cbor")),
                      403);

            const auto got = agg.cli.Get("/keypaths/branch/" + hex(a.uid.bytes) + "/" +
                                         std::to_string(kLeaf));
            ASSERT_TRUE(got && got->status == 200);
            const KeyPath back = Serializer::decode_key_path(
                reinterpret_cast<const uint8_t*>(got->body.data()), got->body.size());
            EXPECT_EQ(KeyPathCert::verify(back), a.uid);
            EXPECT_EQ(agg.status(agg.cli.Get("/keypaths/branch/" + hex(a.uid.bytes) + "/0")),
                      404);
        }
        // Перезапуск помнит выученное: ключи веток собираются из хранилища.
        BranchKeys reloaded;
        reloaded.load(storage);
        EXPECT_EQ(reloaded.check(b), BranchKeys::Verdict::signed_by_branch);
    }
    std::filesystem::remove_all(db);
}

// Между агрегаторами то же правило: путь приходит раньше блоков и проверяется,
// блок своей ветки переходит, подделка, лежащая у соседа, — нет.
TEST(BranchKeys, AggregatorsPassOnlySignedBlocks) {
    Chain a("gossip");
    const auto db_a = std::filesystem::temp_directory_path() / "bc_branch_keys_gossip_a";
    const auto db_b = std::filesystem::temp_directory_path() / "bc_branch_keys_gossip_b";
    std::filesystem::remove_all(db_a);
    std::filesystem::remove_all(db_b);
    const Block good = a.write(a.keys.at(kLeaf));
    const Block fake = forged(a.write(a.keys.at(kLeaf)));
    {
        AggregatorStorage storage_a(db_a), storage_b(db_b);
        Live agg_a(storage_a, 18961);
        const KeyPath kp = a.path();
        ASSERT_EQ(agg_a.status(agg_a.cli.Post("/keypaths/" + hex(KeyPathCert::signer_key(kp).bytes),
                                              cbor(kp), "application/cbor")),
                  200);
        ASSERT_EQ(agg_a.status(agg_a.cli.Post("/blocks", cbor(good), "application/cbor")), 200);
        ASSERT_TRUE(storage_a.add_block(fake));              // лежит у соседа мимо фильтра
        Live agg_b(storage_b, 18962, {"http://127.0.0.1:18961"});
        bool arrived = false;
        for (int i = 0; i < 100 && !arrived; ++i) {
            arrived = storage_b.has_block_hash(Crypto::hash_block(good));
            if (!arrived) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        EXPECT_TRUE(arrived) << "a signed block did not pass within 10s";
        EXPECT_FALSE(storage_b.has_block_hash(Crypto::hash_block(fake)));
    }
    std::filesystem::remove_all(db_a);
    std::filesystem::remove_all(db_b);
}

// Со-подпись слияния лежит рядом с блоком на агрегаторе (blockchain.md §6.4):
// принимается, только если её поставил ключ ветки партнёра, названного в самом
// блоке; у блока, который не слияние, со-подписи не бывает.
TEST(BranchKeys, ACoSignatureComesWithItsMerge) {
    Chain a("cosig_a"), b("cosig_b");
    const auto db = std::filesystem::temp_directory_path() / "bc_branch_keys_cosig_db";
    std::filesystem::remove_all(db);
    {
        AggregatorStorage storage(db);
        Live agg(storage, 18963);
        for (Chain* c : {&a, &b}) {
            const KeyPath kp = c->path();
            ASSERT_EQ(agg.status(agg.cli.Post("/keypaths/" + hex(KeyPathCert::signer_key(kp).bytes),
                                              cbor(kp), "application/cbor")),
                      200);
        }
        const Block a0 = a.write(a.keys.at(kLeaf));
        const Block b0 = b.write(b.keys.at(kLeaf));
        // Половина Алисы: следующий её блок берёт блок Боба.
        MergePayload mp{};
        mp.partner_last_address = b0.address;
        mp.partner_last_hash    = Crypto::hash_block(b0);
        mp.merge_timestamp      = ++a.now;
        Block m{};
        m.address           = {a.uid, kLeaf, a0.address.block_index + 1};
        m.prev_hash         = Crypto::hash_block(a0);
        m.timestamp_claimed = mp.merge_timestamp;
        m.type              = BlockType::MERGE;
        m.payload           = Serializer::encode(mp);
        m = resigned(m, a.keys.at(kLeaf));
        for (const Block* blk : std::initializer_list<const Block*>{&a0, &b0, &m})
            ASSERT_EQ(agg.status(agg.cli.Post("/blocks", cbor(*blk), "application/cbor")), 200);

        const Hash      mh = Crypto::hash_block(m);
        const Signature by_bob =
            Crypto::sign(mh.bytes.data(), mh.bytes.size(), b.keys.at(kLeaf).sec);
        const Signature by_stranger =
            Crypto::sign(mh.bytes.data(), mh.bytes.size(), Crypto::generate_keypair().sec);
        const auto post = [&](const Hash& h, const Signature& s) {
            return agg.status(agg.cli.Post("/cosigs/" + hex(h.bytes),
                                           std::string(s.bytes.begin(), s.bytes.end()),
                                           "application/octet-stream"));
        };
        EXPECT_EQ(post(mh, by_stranger), 403);
        EXPECT_EQ(post(Crypto::hash_block(a0), by_bob), 404);    // не слияние
        EXPECT_EQ(post(mh, by_bob), 200);
        const auto got = agg.cli.Get("/cosigs/" + hex(mh.bytes));
        ASSERT_TRUE(got && got->status == 200);
        EXPECT_EQ(got->body, std::string(by_bob.bytes.begin(), by_bob.bytes.end()));
        EXPECT_TRUE(storage.get_cosig(mh).has_value());
    }
    std::filesystem::remove_all(db);
}
