// Finality of a deal with a window (blockchain.md §11.10): a revocation that came here
// within the window and left the offer off the owner's line voids the deal; a
// fork within the window freezes it until the line is named; after the window
// the deal is final.
#include "aggregator/finality.h"
#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/revocation.h>
#include <blockchain/serializer.h>
#include <blockchain/storage.h>
#include <blockchain/validator.h>
#include <gtest/gtest.h>
#include <ctime>
#include <filesystem>
#include <map>

using namespace aggregator;
using namespace blockchain;

namespace {

constexpr NodeIndex kLeaf   = 0x7FFF'FFFFu;
constexpr NodeIndex kParent = (kLeaf - 1) / 2;

// The buyer's chain as its client keeps it, and the aggregator's table beside it.
struct Scene {
    std::filesystem::path              dir;
    std::unique_ptr<LmdbStorage>       chain_db;
    std::unique_ptr<Validator>         validator;
    std::unique_ptr<Blockchain>        bc;
    std::unique_ptr<AggregatorStorage> table;
    std::map<NodeIndex, KeyPair>       keys;
    UserId                             uid{};
    Timestamp                          clock = 1'800'000'000;
    const Timestamp                    now   = static_cast<Timestamp>(std::time(nullptr));

    explicit Scene(const std::string& name) {
        dir = std::filesystem::temp_directory_path() / ("bc_finality_" + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        chain_db  = std::make_unique<LmdbStorage>(dir / "chain");
        validator = std::make_unique<Validator>(*chain_db);
        bc        = std::make_unique<Blockchain>(*chain_db, *validator);
        table     = std::make_unique<AggregatorStorage>(dir / "table");
        for (NodeIndex idx : path_indices(kLeaf)) keys[idx] = Crypto::generate_keypair();
        uid = keys.at(0).pub;
        bc->create_identity(keys.at(0));
        bc->ensure_path(uid, kLeaf, [&](NodeIndex idx) { return keys.at(idx); });
    }
    ~Scene() {
        table.reset();
        bc.reset();
        validator.reset();
        chain_db.reset();
        std::filesystem::remove_all(dir);
    }
    // A block of the buyer's paying branch, put on the table too.
    Block write() {
        Block b = bc->append_data_block(uid, kLeaf, {0x80}, keys.at(kLeaf), ++clock);
        table->add_block(b);
        return b;
    }
    // The parent revokes the branch key; the certificate comes to the table now.
    void revoke(const std::optional<Hash>& last_good) {
        const Block rev = bc->revoke_node(uid, kLeaf, 1, Crypto::generate_keypair().pub,
                                          kParent, keys.at(kParent), ++clock, last_good);
        const auto cert = RevocationCert::build(*chain_db, rev.address);
        table->put_revocation_bytes(uid, Serializer::encode(cert));
        table->note_arrival(Crypto::hash_block(cert.block));
    }
};

} // namespace

TEST(Finality, WithoutRevocationPendingThenFinal) {
    Scene s("plain");
    const Block offer = s.write();
    EXPECT_EQ(deal_finality(*s.table, offer, s.now - 100, 3600, s.now).state, Finality::pending);
    const auto f = deal_finality(*s.table, offer, s.now - 7200, 3600, s.now);
    EXPECT_EQ(f.state, Finality::final);
    EXPECT_EQ(f.final_at, s.now - 3600);
}

TEST(Finality, ARevocationWithinTheWindowVoids) {
    Scene s("voided");
    const Block offer = s.write();
    s.revoke(std::nullopt);                                  // стоп-кран, линии нет
    const auto f = deal_finality(*s.table, offer, s.now - 100, 3600, s.now);
    EXPECT_EQ(f.state, Finality::voided);
    ASSERT_TRUE(f.revoked_at.has_value());
    EXPECT_LE(*f.revoked_at, f.final_at);
}

TEST(Finality, TheOwnersLineKeepsTheDeal) {
    Scene s("line");
    const Block offer = s.write();
    const Block later = s.write();                           // хозяин писал и после оферты
    s.revoke(Crypto::hash_block(later));                     // линия до later — через оферту
    EXPECT_EQ(deal_finality(*s.table, offer, s.now - 100, 3600, s.now).state, Finality::pending);
}

TEST(Finality, ARevocationAfterTheWindowComesTooLate) {
    Scene s("late");
    const Block offer = s.write();
    s.revoke(std::nullopt);
    EXPECT_EQ(deal_finality(*s.table, offer, s.now - 7200, 3600, s.now).state, Finality::final);
}

TEST(Finality, AForkWithinTheWindowFreezesUntilTheLineIsNamed) {
    Scene s("fork");
    const Block offer = s.write();
    Block rival   = offer;                                   // вор: тот же номер, другой блок
    rival.payload = {0x81, 0x01};
    rival.signature = Signature::null();
    const auto bytes = Serializer::encode(rival);
    rival.signature = Crypto::sign(bytes.data(), bytes.size(), s.keys.at(kLeaf).sec);
    ASSERT_TRUE(s.table->add_block(rival));                  // соперник лёг в сторону
    EXPECT_EQ(deal_finality(*s.table, offer, s.now - 100, 3600, s.now).state, Finality::frozen);

    s.revoke(Crypto::hash_block(offer));                     // предок назвал линию хозяина
    EXPECT_EQ(deal_finality(*s.table, offer, s.now - 100, 3600, s.now).state, Finality::pending);
}
