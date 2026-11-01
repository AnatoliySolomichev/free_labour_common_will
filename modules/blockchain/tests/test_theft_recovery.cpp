// Кражи ключей и восстановление старшими ключами (blockchain.md §6.7, blockchain.md §4.4).
//
// Три хранилища, как в жизни. У хозяина — только его блоки: блок по занятому
// адресу хранилище не примет, поэтому блоков вора у хозяина нет. У вора — копия
// цепи на момент кражи и его продолжения. У проверяющего — сертификаты отзывов,
// как после `bc revoke fetch`: на адресе остаётся пришедший первым.
//
// Путь всех тестов 0 → 1 → 3 → 7: корень не украден никогда, остальные ключи
// воруют в любых сочетаниях и по нескольку раз, включая замены.
//
// Тесты DISABLED_ — расхождения кода с blockchain.md §6.7, найденные этим набором
// (2026-11-01). Ожидания в них — по документу; запуск:
//   blockchain_tests --gtest_also_run_disabled_tests --gtest_filter='TheftRecovery*'

#include "blockchain/blockchain.h"
#include "blockchain/crypto.h"
#include "blockchain/key_path.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>

using namespace blockchain;
namespace fs = std::filesystem;

namespace {

// Хранилище со своим фасадом: хозяин, вор или проверяющий.
struct Store {
    fs::path                     dir;
    std::unique_ptr<LmdbStorage> storage;
    std::unique_ptr<Validator>   validator;
    std::unique_ptr<Blockchain>  bc;

    explicit Store(const std::string& tag) {
        static int cnt = 0;
        dir = fs::temp_directory_path() / ("bc_theft_" + tag + "_" + std::to_string(++cnt));
        fs::remove_all(dir);
        storage   = std::make_unique<LmdbStorage>(dir);
        validator = std::make_unique<Validator>(*storage);
        bc        = std::make_unique<Blockchain>(*storage, *validator);
    }
    ~Store() {
        bc.reset(); validator.reset(); storage.reset();
        fs::remove_all(dir);
    }
};

// Копия вора: все узлы и блоки цепи на момент кражи.
void clone_into(const IStorage& from, IStorage& to, const UserId& chain) {
    std::vector<NodeIndex> nodes;
    from.for_each_node([&](const UserId& c, const Node& n) {
        if (c == chain) nodes.push_back(n.index);
        return true;
    });
    for (NodeIndex idx : nodes) to.put_node(chain, from.get_node(chain, idx));
    for (NodeIndex idx : nodes)
        if (auto tip = from.branch_tip_index(chain, idx))
            for (BlockIndex i = 0; i <= *tip; ++i) to.put_block(from.get_block({chain, idx, i}));
}

// Приём сертификата, как в `bc revoke fetch`: проверка, узлы пути, блок отзыва —
// если адрес ещё свободен. Возвращает, лёг ли блок.
bool import_cert(IStorage& into, const RevocationCertificate& cert) {
    RevocationCert::verify(cert);
    const UserId chain = cert.block.address.user_id;
    for (const auto& node : cert.path)
        if (!into.has_node(chain, node.index)) into.put_node(chain, node);
    if (into.has_block(cert.block.address) || into.has_external_block(cert.block.address))
        return false;
    into.put_external_block(cert.block);
    return true;
}

Block signed_block(const BlockAddress& at, const Hash& prev, Timestamp ts, BlockType type,
                   std::vector<uint8_t> payload, const KeyPair& kp) {
    Block b{};
    b.address           = at;
    b.prev_hash         = prev;
    b.timestamp_claimed = ts;
    b.type              = type;
    b.payload           = std::move(payload);
    b.signature         = Signature::null();
    const auto bytes    = Serializer::encode(b);
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

const std::vector<NodeIndex> kNodes{1, 3, 7};   // всё, кроме корня

// ── Перебор сочетаний ─────────────────────────────────────────────────────────
//
// Раунд — какие ключи украдены разом (их текущие: исходные или замены). Вор
// пишет в каждой украденной ветке свой блок на следующий номер и перехват
// каждого потомка на пути — отзыв с заменой на свой ключ. Хозяин, не зная,
// пишет на тот же номер свой блок, затем узнаёт и отзывает сверху вниз: линия —
// его кончик, замена — свежий ключ — и пишет заменой.

using Rounds = std::vector<std::vector<NodeIndex>>;

enum class Strategy {
    Root,           // каждую украденную ветку отзывает корень
    RootDefensive,  // корень — и украденные, и все ветки, которые вор перехватывал
    Parent,         // радиус по умолчанию — родитель (blockchain.md §6.7 rule 1), его текущим ключом
    Escalate,       // ближайший предок, ни разу не заменённый и не украденный в этом раунде
};

const char* strategy_name(Strategy s) {
    switch (s) {
        case Strategy::Root:          return "root";
        case Strategy::RootDefensive: return "root+defensive";
        case Strategy::Parent:        return "parent";
        case Strategy::Escalate:      return "escalate";
    }
    return "?";
}

std::string rounds_str(const Rounds& rounds) {
    std::string s;
    for (const auto& r : rounds) {
        s += "{";
        for (size_t i = 0; i < r.size(); ++i) s += (i ? "," : "") + std::to_string(r[i]);
        s += "}";
    }
    return s;
}

struct Outcome {
    bool owner = true;     // у хозяина: ветки читаются, его блоки CLEAN, пишет заменой
    bool certs = true;     // отзывы хозяина уходят сертификатами
    bool verifier = true;  // проверяющий с сертификатами вора и хозяина решает за хозяина
    std::string why;
};

// Путь к ветке n из хранилища хозяина с чужим кончиком — так его показал бы вор.
BranchTipInfo tip_with(const IStorage& s, const UserId& uid, const Block& tip_block) {
    const NodeIndex n = tip_block.address.node_index;
    BranchTipInfo tip{};
    tip.tip_address = tip_block.address;
    tip.tip_hash    = Crypto::hash_block(tip_block);
    for (NodeIndex i : path_indices(n)) tip.path.push_back(s.get_node(uid, i));
    tip.tip_block = tip_block;
    tip.births    = births_of(s, uid, n);
    return tip;
}

Outcome play(const Rounds& rounds, Strategy strat) {
    Outcome out;
    Store owner("combo");
    std::map<NodeIndex, KeyPair> cur;
    for (NodeIndex i : {0u, 1u, 3u, 7u}) cur[i] = Crypto::generate_keypair();
    const UserId uid = cur.at(0).pub;
    Timestamp now = 1'000;
    owner.bc->create_identity(cur.at(0));
    owner.bc->ensure_path(uid, 7, [&](NodeIndex i) { return cur.at(i); }, now);

    std::set<NodeIndex> replaced;
    std::vector<Block> owner_records, thief_forks;
    std::vector<RevocationCertificate> thief_certs;
    const auto write = [&](NodeIndex n) {
        owner.bc->append_data_block(uid, n, {0x80}, cur.at(n), ++now);
    };
    for (NodeIndex n : kNodes) write(n);

    for (const auto& round : rounds) {
        // Вор.
        for (NodeIndex n : round) {
            const KeyPair stolen = cur.at(n);
            const auto tip = owner.storage->branch_tip_index(uid, n);
            Block prev = signed_block({uid, n, tip ? *tip + 1 : 0},
                                      owner.bc->branch_tip_hash(uid, n), ++now,
                                      BlockType::DATA, {0x81, 0x01}, stolen);
            thief_forks.push_back(prev);
            for (NodeIndex d : kNodes) {
                if (!is_ancestor(n, d)) continue;
                RevocationPayload rp{};
                rp.revoked_node_index = d;
                rp.revoked_pubkey     = cur.at(d).pub;
                rp.replacement_pubkey = Crypto::generate_keypair().pub;
                BlockAddress at = prev.address;
                at.block_index += 1;
                prev = signed_block(at, Crypto::hash_block(prev), ++now, BlockType::REVOCATION,
                                    Serializer::encode(rp), stolen);
                RevocationCertificate hc{};
                hc.block = prev;
                for (NodeIndex i : path_indices(n))
                    hc.path.push_back(owner.storage->get_node(uid, i));
                hc.births = births_of(*owner.storage, uid, n);
                try {
                    RevocationCert::verify(hc);
                    thief_certs.push_back(hc);
                } catch (const BlockchainError&) {}   // подписан заменой: не уходит, как и у хозяина
            }
        }
        // Хозяин ещё не знает.
        for (NodeIndex n : round) write(n);
        // Хозяин узнал.
        std::vector<NodeIndex> fix = round;
        if (strat == Strategy::RootDefensive)
            for (NodeIndex d : kNodes)
                if (std::find(fix.begin(), fix.end(), d) == fix.end() &&
                    std::any_of(round.begin(), round.end(),
                                [&](NodeIndex n) { return is_ancestor(n, d); }))
                    fix.push_back(d);
        std::sort(fix.begin(), fix.end());
        for (NodeIndex n : fix) {
            NodeIndex a = 0;
            if (strat == Strategy::Parent) a = (n - 1) / 2;
            if (strat == Strategy::Escalate) {
                a = (n - 1) / 2;
                while (a != 0 && (replaced.count(a) > 0 ||
                                  std::find(round.begin(), round.end(), a) != round.end()))
                    a = (a - 1) / 2;
            }
            const KeyPair fresh = Crypto::generate_keypair();
            try {
                owner_records.push_back(owner.bc->revoke_node(
                    uid, n, fresh.pub, a, cur.at(a), ++now, owner.bc->branch_tip_hash(uid, n)));
                cur[n] = fresh;
                replaced.insert(n);
                write(n);
            } catch (const std::exception& e) {
                out.owner = false;
                out.why += "recover " + std::to_string(n) + " by " + std::to_string(a) + ": " +
                           e.what() + "; ";
                return out;
            }
        }
    }

    for (NodeIndex n : kNodes) {
        try {
            const auto s = owner.validator->branch_revocation_status(uid, n);
            for (size_t i = 0; i < s.blocks.size(); ++i)
                if (s.blocks[i] != BlockRevocationStatus::CLEAN) {
                    out.owner = false;
                    out.why += "own " + std::to_string(n) + "#" + std::to_string(i) + " not clean; ";
                }
            owner.validator->validate_branch(uid, n);
        } catch (const std::exception& e) {
            out.owner = false;
            out.why += "own " + std::to_string(n) + ": " + e.what() + "; ";
        }
    }

    std::vector<RevocationCertificate> owner_certs;
    for (const Block& r : owner_records) {
        try {
            auto c = RevocationCert::build(*owner.storage, r.address);
            RevocationCert::verify(c);
            owner_certs.push_back(c);
        } catch (const std::exception& e) {
            out.certs = false;
            out.why += "cert " + std::to_string(r.address.node_index) + "#" +
                       std::to_string(r.address.block_index) + ": " + e.what() + "; ";
        }
    }

    // Вор выкладывает свои сертификаты раньше: хозяин о краже ещё не знает.
    Store v("combo_verifier");
    for (const auto& c : thief_certs) import_cert(*v.storage, c);
    for (const auto& c : owner_certs) import_cert(*v.storage, c);
    for (NodeIndex n : kNodes) {
        const auto st = v.validator->effective_revocation(uid, n);
        const bool want = replaced.count(n) > 0;
        if (st.has_value() != want ||
            (want && !(st->replacement_pubkey && *st->replacement_pubkey == cur.at(n).pub))) {
            out.verifier = false;
            out.why += "verifier: " + std::to_string(n) + " not the owner's; ";
        }
        try {
            v.validator->check_tip_against_revocations(
                MergeSession(*owner.storage, *owner.validator).prepare_tip(uid, n));
        } catch (const std::exception& e) {
            out.verifier = false;
            out.why += "verifier refuses owner's tip " + std::to_string(n) + ": " + e.what() + "; ";
        }
    }
    for (const Block& f : thief_forks) {
        bool refused = false;
        try { v.validator->check_tip_against_revocations(tip_with(*owner.storage, uid, f)); }
        catch (const RevocationError&) { refused = true; }
        if (!refused) {
            out.verifier = false;
            out.why += "verifier takes thief's tip " + std::to_string(f.address.node_index) + "#" +
                       std::to_string(f.address.block_index) + "; ";
        }
    }
    return out;
}

// Все последовательности раундов длиной 1..max_rounds из 7 непустых подмножеств {1, 3, 7}.
std::vector<Rounds> combos(size_t max_rounds) {
    const std::vector<std::vector<NodeIndex>> subsets{
        {1}, {3}, {7}, {1, 3}, {1, 7}, {3, 7}, {1, 3, 7}};
    std::vector<Rounds> out;
    std::vector<Rounds> layer{{}};
    for (size_t r = 0; r < max_rounds; ++r) {
        std::vector<Rounds> next;
        for (const auto& prefix : layer)
            for (const auto& s : subsets) {
                Rounds c = prefix;
                c.push_back(s);
                next.push_back(c);
            }
        out.insert(out.end(), next.begin(), next.end());
        layer = std::move(next);
    }
    return out;
}

class TheftRecovery : public ::testing::Test {
protected:
    std::unique_ptr<Store> owner_;
    KeyPair   root_, k1_, k3_, k7_;
    UserId    uid_{};
    Timestamp now_ = 1'000;

    void SetUp() override {
        owner_ = std::make_unique<Store>("owner");
        root_  = Crypto::generate_keypair();
        k1_    = Crypto::generate_keypair();
        k3_    = Crypto::generate_keypair();
        k7_    = Crypto::generate_keypair();
        uid_   = root_.pub;
        std::map<NodeIndex, KeyPair> keys{{0, root_}, {1, k1_}, {3, k3_}, {7, k7_}};
        owner_->bc->create_identity(root_);
        owner_->bc->ensure_path(uid_, 7, [&](NodeIndex i) { return keys.at(i); }, now_);
    }
    void TearDown() override { owner_.reset(); }

    Blockchain&  bc()  { return *owner_->bc; }
    Validator&   val() { return *owner_->validator; }
    LmdbStorage& st()  { return *owner_->storage; }

    Block data(NodeIndex n, const KeyPair& kp) {
        return bc().append_data_block(uid_, n, {0x80}, kp, ++now_);
    }
    Block revoke(NodeIndex n, std::optional<PublicKey> repl, NodeIndex author,
                 const KeyPair& akp, std::optional<Hash> last_good = std::nullopt) {
        return bc().revoke_node(uid_, n, repl, author, akp, ++now_, last_good);
    }
    RevocationCertificate cert(const Block& rev) {
        return RevocationCert::build(st(), rev.address);
    }
    void expect_all_clean(NodeIndex n) {
        const auto s = val().branch_revocation_status(uid_, n);
        for (size_t i = 0; i < s.blocks.size(); ++i)
            EXPECT_EQ(s.blocks[i], BlockRevocationStatus::CLEAN) << "branch " << n << " #" << i;
        EXPECT_NO_THROW(val().validate_branch(uid_, n)) << "branch " << n;
    }
};

} // namespace

// ── Что работает ──────────────────────────────────────────────────────────────

// Корень отзывает ветку снова и снова: крадут каждую его замену.
TEST_F(TheftRecovery, TheRootRecoversRepeatedTheftsOfItsReplacements) {
    data(7, k7_);
    KeyPair cur = k7_;
    std::vector<Block> revs;
    for (int round = 0; round < 4; ++round) {
        const Block mine = data(7, cur);
        const KeyPair next = Crypto::generate_keypair();
        revs.push_back(revoke(7, next.pub, 0, root_, Crypto::hash_block(mine)));
        cur = next;
    }
    data(7, cur);
    const auto s = val().branch_revocation_status(uid_, 7);
    EXPECT_EQ(s.state, BranchRevocationState::REPLACED);
    ASSERT_TRUE(s.next_key.has_value());
    EXPECT_EQ(*s.next_key, cur.pub);
    expect_all_clean(7);

    Store v("verifier");
    for (const Block& r : revs) import_cert(*v.storage, cert(r));
    const auto e = v.validator->effective_revocation(uid_, 7);
    ASSERT_TRUE(e.has_value());
    ASSERT_TRUE(e->replacement_pubkey.has_value());
    EXPECT_EQ(*e->replacement_pubkey, cur.pub);
}

// Украдено всё, кроме корня, разом: корень отзывает 1, 3 и 7, каждую с линией.
TEST_F(TheftRecovery, AllButTheRootStolenAtOnce) {
    data(1, k1_);
    data(3, k3_);
    data(7, k7_);
    const Block stolen3 = signed_block({uid_, 3, *st().branch_tip_index(uid_, 3) + 1},
                                       bc().branch_tip_hash(uid_, 3), ++now_, BlockType::DATA,
                                       {0x81, 0x01}, k3_);      // блок вора старым ключом
    std::map<NodeIndex, KeyPair> repl;
    std::vector<Block> revs;
    for (NodeIndex n : kNodes) {
        repl[n] = Crypto::generate_keypair();
        revs.push_back(revoke(n, repl.at(n).pub, 0, root_, bc().branch_tip_hash(uid_, n)));
    }
    for (NodeIndex n : kNodes) {
        EXPECT_NO_THROW(data(n, repl.at(n)));
        expect_all_clean(n);
    }

    Store v("verifier");
    for (const Block& r : revs) EXPECT_TRUE(import_cert(*v.storage, cert(r)));
    for (NodeIndex n : kNodes) {
        const auto e = v.validator->effective_revocation(uid_, n);
        ASSERT_TRUE(e.has_value());
        ASSERT_TRUE(e->replacement_pubkey.has_value());
        EXPECT_EQ(*e->replacement_pubkey, repl.at(n).pub);
        EXPECT_NO_THROW(v.validator->check_tip_against_revocations(
            MergeSession(st(), val()).prepare_tip(uid_, n)));
    }
    EXPECT_THROW(v.validator->check_tip_against_revocations(tip_with(st(), uid_, stolen3)),
                 RevocationError);
}

// Главная проверка: если все отзывы пишет корень, а ветки, которые вор
// перехватывал отзывами из украденных предков, корень тоже переводит на свежие
// замены, — любое сочетание краж до двух раундов (и самые длинные цепочки из трёх)
// восстанавливается: у хозяина, в сертификатах и у проверяющего.
TEST_F(TheftRecovery, EveryCombinationRecoversByTheRootAlone) {
    std::vector<Rounds> all = combos(2);
    all.push_back({{7}, {7}, {7}});
    all.push_back({{1, 3, 7}, {1, 3, 7}, {1, 3, 7}});
    all.push_back({{7}, {3}, {1}});
    all.push_back({{1}, {3}, {7}});
    for (const auto& c : all) {
        const Outcome o = play(c, Strategy::RootDefensive);
        EXPECT_TRUE(o.owner && o.certs && o.verifier) << rounds_str(c) << ": " << o.why;
    }
}

// Обход у себя: украденного родителя отзывает корень, дальше родитель действует
// заменой. В своём хранилище это работает; уйти сертификатом не может
// (DISABLED_ARecordOfAReplacedAuthorTravels).
TEST_F(TheftRecovery, AStolenParentActsThroughItsReplacementLocally) {
    const Block b0 = data(7, k7_);
    const KeyPair r7a = Crypto::generate_keypair();
    revoke(7, r7a.pub, 3, k3_, Crypto::hash_block(b0));
    const Block b1 = data(7, r7a);
    // Украли ключ 3 и R7a.
    const KeyPair r3 = Crypto::generate_keypair();
    revoke(3, r3.pub, 0, root_, bc().branch_tip_hash(uid_, 3));
    const KeyPair r7b = Crypto::generate_keypair();
    revoke(7, r7b.pub, 3, r3, Crypto::hash_block(b1));
    EXPECT_NO_THROW(data(7, r7b));
    expect_all_clean(7);
}

// Перехват из украденного предка за линией, которую назвал корень, у
// проверяющего не считается (blockchain.md §6.7 rule 10).
TEST_F(TheftRecovery, AHijackPastTheRootsLineDoesNotCount) {
    data(1, k1_);                                                // #0 рождение 3, #1
    Store thief("thief");
    clone_into(st(), *thief.storage, uid_);                      // украден ключ 1
    thief.bc->append_data_block(uid_, 1, {0x81}, k1_, 5'000LL);  // #2 вора
    const KeyPair tk = Crypto::generate_keypair();
    const Block hijack = thief.bc->revoke_node(uid_, 7, tk.pub, 1, k1_, 5'001LL);   // #3 вора
    const Block mine = data(1, k1_);                             // #2 хозяина
    const KeyPair r1 = Crypto::generate_keypair();
    const Block g = revoke(1, r1.pub, 0, root_, Crypto::hash_block(mine));

    Store v("verifier");
    EXPECT_TRUE(import_cert(*v.storage, RevocationCert::build(*thief.storage, hijack.address)));
    EXPECT_TRUE(v.validator->effective_revocation(uid_, 7).has_value());   // пока работает
    import_cert(*v.storage, cert(g));
    EXPECT_FALSE(v.validator->effective_revocation(uid_, 7).has_value());
}

// Проверяющий, у которого есть конечный блок линии, проходит её назад: ложный
// потомок, рождённый вором на номере внутри линии, не стоит (blockchain.md §6.7 rule 6).
TEST_F(TheftRecovery, TheVerifierWithTheLineEndRefusesAFakeChild) {
    data(7, k7_);                                                // #0
    Store thief("thief");
    clone_into(st(), *thief.storage, uid_);                      // украден K7
    const KeyPair k15 = Crypto::generate_keypair();
    thief.bc->ensure_path(uid_, 15, [&](NodeIndex i) { return i == 15 ? k15 : k7_; }, 6'000LL);
    thief.bc->append_data_block(uid_, 15, {0x80}, k15, 6'001LL);
    const BranchTipInfo fake = MergeSession(*thief.storage, *thief.validator).prepare_tip(uid_, 15);
    ASSERT_EQ(fake.births.back().address.block_index, 1u);       // рождение вора — #1
    data(7, k7_);                                                // #1 хозяина
    const Block end = data(7, k7_);                              // #2 хозяина
    const KeyPair r7 = Crypto::generate_keypair();
    const Block rev = revoke(7, r7.pub, 3, k3_, Crypto::hash_block(end));

    Store v("verifier");
    import_cert(*v.storage, cert(rev));
    v.storage->put_external_block(end);
    EXPECT_THROW(v.validator->check_tip_against_revocations(fake), RevocationError);
}

// После стоп-крана родителя корень назначил замену без линии — линия родителя
// пропала (DISABLED_AHigherAncestorKeepsTheLowerAncestorsLine). Через фасад её
// можно вернуть только обходом: заморозить и назвать старый ключ заново.
TEST_F(TheftRecovery, AFrozenBranchLetsTheRootNameTheOldKeyAgain) {
    const Block b0 = data(7, k7_);
    data(7, k7_);
    revoke(7, std::nullopt, 3, k3_, Crypto::hash_block(b0));
    const KeyPair r = Crypto::generate_keypair();
    revoke(7, r.pub, 0, root_);
    const Block f = revoke(7, std::nullopt, 0, root_, Crypto::hash_block(b0));   // называет R
    EXPECT_EQ(Serializer::decode_revocation_payload(f.payload.data(), f.payload.size()).revoked_pubkey,
              r.pub);
    EXPECT_EQ(val().branch_revocation_status(uid_, 7).state, BranchRevocationState::FROZEN);
    const KeyPair r2 = Crypto::generate_keypair();
    const Block g = revoke(7, r2.pub, 0, root_, Crypto::hash_block(b0));       // теперь K7
    EXPECT_EQ(Serializer::decode_revocation_payload(g.payload.data(), g.payload.size()).revoked_pubkey,
              k7_.pub);
    const auto s = val().branch_revocation_status(uid_, 7);
    EXPECT_EQ(s.state, BranchRevocationState::REPLACED);
    EXPECT_EQ(s.blocks[0], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(s.blocks[1], BlockRevocationStatus::OUTSIDE_LINE);
}

// ── Расхождения с blockchain.md §6.7 (2026-11-01) ─────────────────────────────

// Повторная кража с эскалацией (blockchain.md §6.7 rules 3, 9). Родитель заменил
// K7 на R7a, хозяин писал под R7a; R7a украли (с ним ключ 3), и отзыв пишет
// предок выше. Код берёт записи только самого высокого автора
// (Validator::winning_records): эпоха K7 → R7a, названная родителем, пропадает,
// блок под R7a не приписан ни одному ключу — SignatureError на каждом обходе.
// Ветка не пишется, не проверяется, и фасад её уже не чинит: revoke_node сам
// обходит ветку и падает. Ожидание: эпохи нижнего автора сохраняются, пока
// верхний их не перекрыл.
TEST_F(TheftRecovery, DISABLED_EscalationKeepsTheLowerAncestorsEpochs) {
    const auto escalate_to = [&](NodeIndex higher, const KeyPair& higher_kp) {
        SCOPED_TRACE(higher);
        const Block b0 = data(7, k7_);
        const KeyPair r7a = Crypto::generate_keypair();
        revoke(7, r7a.pub, 3, k3_, Crypto::hash_block(b0));
        const Block b1 = data(7, r7a);
        const KeyPair r7b = Crypto::generate_keypair();
        revoke(7, r7b.pub, higher, higher_kp, Crypto::hash_block(b1));
        ASSERT_NO_THROW(val().branch_revocation_status(uid_, 7));
        EXPECT_NO_THROW(data(7, r7b));
        expect_all_clean(7);
    };
    escalate_to(1, k1_);       // деду
    TearDown();
    SetUp();
    escalate_to(0, root_);     // корню
}

// Тот же корень беды мягче: родитель остановил K7 и назвал линию, корень
// назначил замену без линии — линия родителя пропала, блоки снова «под вопросом».
TEST_F(TheftRecovery, DISABLED_AHigherAncestorKeepsTheLowerAncestorsLine) {
    const Block b0 = data(7, k7_);
    data(7, k7_);
    revoke(7, std::nullopt, 3, k3_, Crypto::hash_block(b0));
    const KeyPair r = Crypto::generate_keypair();
    revoke(7, r.pub, 0, root_);
    const auto s = val().branch_revocation_status(uid_, 7);
    ASSERT_EQ(s.blocks.size(), 2u);
    EXPECT_EQ(s.blocks[0], BlockRevocationStatus::CLEAN);
    EXPECT_EQ(s.blocks[1], BlockRevocationStatus::OUTSIDE_LINE);
}

// Запись автора, работающего под заменой, не уходит сертификатом:
// RevocationCert::verify сверяет подпись с ключом в узле автора (ограничение
// записано в blockchain.md §11.2). Вместе с первым расхождением это значит: после
// кражи всех ключей, кроме корня, наружу может писать только корень.
TEST_F(TheftRecovery, DISABLED_ARecordOfAReplacedAuthorTravels) {
    data(7, k7_);
    const KeyPair r3 = Crypto::generate_keypair();
    revoke(3, r3.pub, 0, root_, bc().branch_tip_hash(uid_, 3));
    const Block rec = revoke(7, std::nullopt, 3, r3, bc().branch_tip_hash(uid_, 7));
    ASSERT_TRUE(val().effective_revocation(uid_, 7).has_value());
    EXPECT_NO_THROW(RevocationCert::verify(cert(rec)));
}

// Проверяющий судит чужую запись автора под отзывом по номеру
// (Validator::author_record_counts): на самом номере конца линии отпечаток не
// сверяется. Вор с ключом 3 пишет перехват на тот же номер, что и запись
// хозяина; у проверяющего первым лёг перехват (запись хозяина на занятый адрес
// уже не ляжет), корень называет линией запись хозяина — а перехват считается.
TEST_F(TheftRecovery, DISABLED_AnImportedRecordAtTheLineEndIsCheckedByHash) {
    const Block b0 = data(7, k7_);
    Store thief("thief");
    clone_into(st(), *thief.storage, uid_);                      // украден ключ 3
    const KeyPair tk = Crypto::generate_keypair();
    const Block hijack = thief.bc->revoke_node(uid_, 7, tk.pub, 3, k3_, 5'000LL);
    const KeyPair r7 = Crypto::generate_keypair();               // и K7 украден
    const Block own = revoke(7, r7.pub, 3, k3_, Crypto::hash_block(b0));
    ASSERT_EQ(own.address, hijack.address);
    const Block g = revoke(3, std::nullopt, 0, root_, Crypto::hash_block(own));

    Store v("verifier");
    import_cert(*v.storage, RevocationCert::build(*thief.storage, hijack.address));
    EXPECT_FALSE(import_cert(*v.storage, cert(own)));            // адрес занят
    import_cert(*v.storage, cert(g));
    const auto e = v.validator->effective_revocation(uid_, 7);
    // Перехват вне линии; записи хозяина у проверяющего нет — отзыва нет вовсе.
    EXPECT_FALSE(e.has_value() && e->replacement_pubkey.has_value() &&
                 *e->replacement_pubkey == tk.pub);
}

// Ниже конца линии проверяющий тоже судит по номеру — даже когда конечный блок
// линии у него есть и по нему видно, что на этом номере у хозяина другой блок
// (Validator::birth_stands в том же положении линию проходит).
TEST_F(TheftRecovery, DISABLED_AnImportedRecordBelowTheLineEndIsWalked) {
    data(3, k3_);                                                // #0 рождение 7, #1
    Store thief("thief");
    clone_into(st(), *thief.storage, uid_);                      // украден ключ 3
    const KeyPair tk = Crypto::generate_keypair();
    const Block hijack = thief.bc->revoke_node(uid_, 7, tk.pub, 3, k3_, 5'000LL);   // #2 вора
    ASSERT_EQ(hijack.address.block_index, 2u);
    data(3, k3_);                                                // #2 хозяина, не зная
    const Block end = data(3, k3_);                              // #3 хозяина
    const KeyPair r3 = Crypto::generate_keypair();
    const Block g = revoke(3, r3.pub, 0, root_, Crypto::hash_block(end));

    Store v("verifier");
    import_cert(*v.storage, RevocationCert::build(*thief.storage, hijack.address));
    import_cert(*v.storage, cert(g));
    v.storage->put_external_block(end);                          // prev_hash ведёт не к перехвату
    EXPECT_FALSE(val().effective_revocation(uid_, 7).has_value());
    EXPECT_FALSE(v.validator->effective_revocation(uid_, 7).has_value());
}

// Запись автора под отзывом проверяющий меряет последней названной линией
// автора — какой бы эпохой ни была подписана запись (RevocationState.last_good —
// сводка по всем ключам). Ключ 1 украли, корень назвал линию #2, перехват вора
// на #3 отрезан. Украли и замену R1, корень назвал её линию #4 — и перехват,
// подписанный ещё первым ключом, ожил: 3 ≤ 4.
TEST_F(TheftRecovery, DISABLED_AnImportedRecordIsJudgedByItsOwnEpochsLine) {
    data(1, k1_);                                                // #0 рождение 3, #1
    Store thief("thief");
    clone_into(st(), *thief.storage, uid_);                      // украден ключ 1
    thief.bc->append_data_block(uid_, 1, {0x81}, k1_, 5'000LL);  // #2 вора
    const KeyPair tk = Crypto::generate_keypair();
    const Block hijack = thief.bc->revoke_node(uid_, 7, tk.pub, 1, k1_, 5'001LL);   // #3 вора
    const Block mine2 = data(1, k1_);                            // #2 хозяина
    const KeyPair r1 = Crypto::generate_keypair();
    const Block g1 = revoke(1, r1.pub, 0, root_, Crypto::hash_block(mine2));
    data(1, r1);                                                 // #3 хозяина (R1)
    const Block mine4 = data(1, r1);                             // #4 хозяина (R1)
    const KeyPair r1b = Crypto::generate_keypair();              // украдена R1
    const Block g2 = revoke(1, r1b.pub, 0, root_, Crypto::hash_block(mine4));

    Store v("verifier");
    import_cert(*v.storage, RevocationCert::build(*thief.storage, hijack.address));
    import_cert(*v.storage, cert(g1));
    ASSERT_FALSE(v.validator->effective_revocation(uid_, 7).has_value());
    import_cert(*v.storage, cert(g2));
    EXPECT_FALSE(v.validator->effective_revocation(uid_, 7).has_value());
}

// Та же сводка в своём хранилище: Validator::birth_stands меряет рождение
// последней названной линией. Замену R7a украли, не успев ею ничего подписать,
// и хозяин честно назвал её линию «ни одного верного блока» — и потомок,
// рождённый на линии K7, стал недействительным (blockchain.md §6.7 rule 6).
TEST_F(TheftRecovery, DISABLED_ALaterEpochWithNoValidBlockKeepsEarlierBirths) {
    const KeyPair k15 = Crypto::generate_keypair();
    data(7, k7_);
    bc().ensure_path(uid_, 15, [&](NodeIndex i) { return i == 15 ? k15 : k7_; }, ++now_);
    const Block b2 = data(7, k7_);
    const KeyPair r7a = Crypto::generate_keypair();
    revoke(7, r7a.pub, 3, k3_, Crypto::hash_block(b2));
    ASSERT_FALSE(val().node_invalidated_by_revocation(uid_, 15));
    const KeyPair r7b = Crypto::generate_keypair();
    revoke(7, r7b.pub, 3, k3_, Crypto::hash_node(st().get_node(uid_, 7)));
    expect_all_clean(7);                                         // ветка сама — в порядке
    EXPECT_FALSE(val().node_invalidated_by_revocation(uid_, 15));
}

// Сводка по стратегиям восстановления — без проверок, печатает таблицу:
// все сочетания краж в 1–3 раунда (399) × 4 стратегии. Долго, около минуты.
TEST_F(TheftRecovery, DISABLED_StrategyMatrix) {
    const auto all = combos(3);
    for (Strategy s : {Strategy::Root, Strategy::RootDefensive, Strategy::Parent,
                       Strategy::Escalate}) {
        int owner = 0, certs = 0, verifier = 0, total = 0;
        for (const auto& c : all) {
            const Outcome o = play(c, s);
            ++total;
            owner += o.owner;
            certs += o.certs;
            verifier += o.verifier;
        }
        std::printf("%-15s total %3d  owner %3d  certs %3d  verifier %3d\n",
                    strategy_name(s), total, owner, certs, verifier);
    }
}
