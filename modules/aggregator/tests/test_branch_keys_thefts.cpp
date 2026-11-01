// Повторные кражи ключей глазами агрегатора (sync.md §10.10, blockchain.md §6.7):
// чьи блоки BranchKeys пускает на склад и чьими их считает.
//
// Путь 0 → 1 → 3 → 7: корень не украден никогда, остальные ключи воруют в любых
// сочетаниях и по нескольку раз. Вор выкладывает всё раньше хозяина: свои блоки,
// перехваты потомков и ложных потомков.
//
// Тесты DISABLED_ — расхождения с blockchain.md §6.7, найденные этим набором
// (2026-11-01); ожидания — по документу. Запуск:
//   aggregator_tests --gtest_also_run_disabled_tests --gtest_filter='BranchKeysThefts*'

#include "aggregator/branch_keys.h"

#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/key_path.h>
#include <blockchain/serializer.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>

using namespace aggregator;
using namespace blockchain;
namespace fs = std::filesystem;

namespace {

// Своя цепь хозяина: путь 0 → 1 → 3 → 7, по ветке у каждого узла пути.
struct Owner {
    fs::path                     dir;
    std::unique_ptr<LmdbStorage> storage;
    std::unique_ptr<Validator>   validator;
    std::unique_ptr<Blockchain>  bc;
    std::map<NodeIndex, KeyPair> keys;
    UserId                       uid{};
    Timestamp                    now = 1'000;

    explicit Owner(const std::string& tag) {
        static int cnt = 0;
        dir = fs::temp_directory_path() / ("bc_bk_thefts_" + tag + "_" + std::to_string(++cnt));
        fs::remove_all(dir);
        fs::create_directories(dir);
        storage   = std::make_unique<LmdbStorage>(dir / "db");
        validator = std::make_unique<Validator>(*storage);
        bc        = std::make_unique<Blockchain>(*storage, *validator);
        for (NodeIndex i : {0u, 1u, 3u, 7u}) keys[i] = Crypto::generate_keypair();
        uid = keys.at(0).pub;
        bc->create_identity(keys.at(0));
        bc->ensure_path(uid, 7, [&](NodeIndex i) { return keys.at(i); }, now);
    }
    ~Owner() {
        bc.reset(); validator.reset(); storage.reset();
        fs::remove_all(dir);
    }

    Block data(NodeIndex n, const KeyPair& kp) {
        return bc->append_data_block(uid, n, {0x80}, kp, ++now);
    }
    Block revoke(NodeIndex n, std::optional<PublicKey> repl, NodeIndex author,
                 const KeyPair& akp, std::optional<Hash> last_good = std::nullopt) {
        return bc->revoke_node(uid, n, repl, author, akp, ++now, last_good);
    }
    RevocationCertificate cert(const Block& rev) const {
        return RevocationCert::build(*storage, rev.address);
    }
    // Все блоки всех веток пути — как хозяин выложил бы их на агрегатор.
    std::vector<Block> all_blocks() const {
        std::vector<Block> out;
        for (NodeIndex n : {0u, 1u, 3u, 7u})
            if (auto tip = storage->branch_tip_index(uid, n))
                for (BlockIndex i = 0; i <= *tip; ++i)
                    out.push_back(storage->get_block({uid, n, i}));
        return out;
    }
};

// Склад агрегатора во временном каталоге.
struct Agg {
    fs::path                           dir;
    std::unique_ptr<AggregatorStorage> storage;
    explicit Agg(const std::string& tag) {
        static int cnt = 0;
        dir = fs::temp_directory_path() / ("bc_bk_thefts_agg_" + tag + "_" + std::to_string(++cnt));
        fs::remove_all(dir);
        storage = std::make_unique<AggregatorStorage>(dir);
    }
    ~Agg() {
        storage.reset();
        fs::remove_all(dir);
    }
};

Block signed_by(Block b, const KeyPair& kp) {
    b.signature      = Signature::null();
    const auto bytes = Serializer::encode(b);
    b.signature      = Crypto::sign(bytes.data(), bytes.size(), kp.sec);
    return b;
}

Block block_after(const BlockAddress& at, const Hash& prev, Timestamp ts, BlockType type,
                  std::vector<uint8_t> payload, const KeyPair& kp) {
    Block b{};
    b.address           = at;
    b.prev_hash         = prev;
    b.timestamp_claimed = ts;
    b.type              = type;
    b.payload           = std::move(payload);
    return signed_by(b, kp);
}

// Сертификат записи, которой нет в хранилище хозяина: запись вора строится так же.
RevocationCertificate cert_of(const Owner& o, const Block& rec) {
    RevocationCertificate c{};
    c.block = rec;
    for (NodeIndex i : path_indices(rec.address.node_index))
        c.path.push_back(o.storage->get_node(o.uid, i));
    c.births = births_of(*o.storage, o.uid, rec.address.node_index);
    return c;
}

const std::vector<NodeIndex> kNodes{1, 3, 7};

// ── Перебор сочетаний ─────────────────────────────────────────────────────────
//
// Как в modules/blockchain/tests/test_theft_recovery.cpp: раунд — какие текущие
// ключи украдены разом. Вор пишет свой блок на следующий номер украденной ветки,
// перехват каждого потомка на пути и рождает ложного потомка на свободном месте
// (с блоком в его ветке). Хозяин, не зная, пишет на тот же номер, затем отзывает
// сверху вниз — линия его кончик, замена свежая — и пишет заменой.

using Rounds = std::vector<std::vector<NodeIndex>>;

// Те же стратегии, что в наборе ядра.
enum class Strategy { Root, RootDefensive, Parent, Escalate };

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

// Свободное место под ложного потомка: второй ребёнок узла, не лежащий на пути.
NodeIndex free_child(NodeIndex n) { return n == 7 ? 15 : 2 * n + 2; }

struct Result {
    std::string why;              // пусто — агрегатор решил всё за хозяина
    int         fakes_known = 0;  // ложных потомков, чей путь агрегатор принял
};

Result play(const Rounds& rounds, Strategy strat) {
    Result res;
    std::string& why = res.why;
    Owner o("combo");
    std::map<NodeIndex, KeyPair> cur = o.keys;
    std::set<NodeIndex> replaced;
    std::vector<Block> owner_data, owner_records, thief_blocks, thief_data;
    std::vector<RevocationCertificate> thief_certs;
    std::vector<KeyPath> fake_paths;
    for (NodeIndex n : kNodes) owner_data.push_back(o.data(n, cur.at(n)));

    bool broken = false;   // своё хранилище хозяина сломалось — дальше он не пишет
    for (const auto& round : rounds) {
        if (broken) break;
        for (NodeIndex n : round) {
            const KeyPair stolen = cur.at(n);
            const auto tip = o.storage->branch_tip_index(o.uid, n);
            Block prev = block_after({o.uid, n, tip ? *tip + 1 : 0}, o.bc->branch_tip_hash(o.uid, n),
                                     ++o.now, BlockType::DATA, {0x81, 0x01}, stolen);
            thief_blocks.push_back(prev);
            thief_data.push_back(prev);
            const auto next_at = [&] {
                BlockAddress at = prev.address;
                at.block_index += 1;
                return at;
            };
            for (NodeIndex d : kNodes) {
                if (!is_ancestor(n, d)) continue;
                RevocationPayload rp{};
                rp.revoked_node_index = d;
                rp.revoked_pubkey     = cur.at(d).pub;
                rp.replacement_pubkey = Crypto::generate_keypair().pub;
                prev = block_after(next_at(), Crypto::hash_block(prev), ++o.now,
                                   BlockType::REVOCATION, Serializer::encode(rp), stolen);
                thief_blocks.push_back(prev);
                thief_certs.push_back(cert_of(o, prev));
            }
            // Ложный потомок.
            const NodeIndex c = free_child(n);
            const KeyPair kc = Crypto::generate_keypair();
            prev = block_after(next_at(), Crypto::hash_block(prev), ++o.now, BlockType::BIRTH,
                               Serializer::encode(BirthPayload{c, kc.pub, kc.pub}), stolen);
            thief_blocks.push_back(prev);
            Node child{};
            child.index             = c;
            child.structural_pubkey = kc.pub;
            child.working_pubkey    = kc.pub;
            child.parent_hash       = Crypto::hash_node(o.storage->get_node(o.uid, n));
            child.birth             = BirthRef{prev.address.block_index, Crypto::hash_block(prev)};
            child.parent_sig        = Signature::null();
            const auto nb           = Serializer::encode(child);
            child.parent_sig        = Crypto::sign(nb.data(), nb.size(), stolen.sec);
            KeyPath kp{};
            for (NodeIndex i : path_indices(n)) kp.path.push_back(o.storage->get_node(o.uid, i));
            kp.path.push_back(child);
            kp.births = births_of(*o.storage, o.uid, n);
            kp.births.push_back(prev);
            fake_paths.push_back(kp);
            const Block fake = block_after({o.uid, c, 0}, Crypto::hash_node(child), ++o.now,
                                           BlockType::DATA, {0x81, 0x02}, kc);
            thief_blocks.push_back(fake);
            thief_data.push_back(fake);
        }
        for (NodeIndex n : round) owner_data.push_back(o.data(n, cur.at(n)));
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
                owner_records.push_back(
                    o.revoke(n, fresh.pub, a, cur.at(a), o.bc->branch_tip_hash(o.uid, n)));
                cur[n] = fresh;
                replaced.insert(n);
                owner_data.push_back(o.data(n, cur.at(n)));
            } catch (const std::exception& e) {
                // Своё хранилище хозяина сломано — это проверяет набор ядра; здесь
                // дальше считаем, что хозяин успел выложить, что успел.
                why += std::string("[owner: ") + e.what() + "] ";
                broken = true;
                break;
            }
        }
    }

    Agg agg("combo");
    for (const Block& b : thief_blocks) agg.storage->add_block(b);   // вор — первым
    for (const Block& b : o.all_blocks()) agg.storage->add_block(b);

    BranchKeys keys;
    for (NodeIndex n : kNodes) keys.learn_path(KeyPathCert::build(*o.storage, o.uid, n));
    for (const KeyPath& kp : fake_paths) {
        try {
            keys.learn_path(kp);
            ++res.fakes_known;
        } catch (const std::exception&) {}   // рождён заменой: путь не сходится
    }
    for (const auto& c : thief_certs) {
        try { keys.learn_revocation(c); } catch (const std::exception&) {}
    }
    for (const Block& r : owner_records) {
        try { keys.learn_revocation(o.cert(r)); } catch (const std::exception&) {}
    }

    for (const Block& b : owner_data) {
        const auto v = keys.check(b, agg.storage.get());
        if (v != BranchKeys::Verdict::signed_by_branch ||
            keys.whose(b, *agg.storage) != BranchKeys::Whose::owner)
            why += "owner's " + std::to_string(b.address.node_index) + "#" +
                   std::to_string(b.address.block_index) + " refused; ";
    }
    for (const Block& b : thief_data) {
        if (keys.check(b, agg.storage.get()) == BranchKeys::Verdict::signed_by_branch ||
            keys.whose(b, *agg.storage) == BranchKeys::Whose::owner)
            why += "thief's " + std::to_string(b.address.node_index) + "#" +
                   std::to_string(b.address.block_index) + " taken; ";
    }
    return res;
}

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

} // namespace

// Все отзывы пишет корень — и агрегатор в любом сочетании краж до двух раундов (и
// в самых длинных цепочках из трёх) пускает блоки хозяина и не пускает блоки
// вора, его перехваты и ложных потомков. Защитных отзывов неукраденных веток
// агрегатору не нужно: перехват из предка он отрезает по линии предка (blockchain.md §6.7 rule 10).
TEST(BranchKeysThefts, TheRootAloneRecoversEveryCombination) {
    std::vector<Rounds> all = combos(2);
    all.push_back({{7}, {7}, {7}});
    all.push_back({{1, 3, 7}, {1, 3, 7}, {1, 3, 7}});
    all.push_back({{7}, {3}, {1}});
    all.push_back({{1}, {3}, {7}});
    int fakes = 0;
    for (const auto& c : all) {
        const Result r = play(c, Strategy::Root);
        EXPECT_TRUE(r.why.empty()) << rounds_str(c) << ": " << r.why;
        fakes += r.fakes_known;
    }
    EXPECT_GT(fakes, 0);   // ложные потомки и правда дошли до проверки
}

// Эскалация после использованной замены (blockchain.md §6.7 rules 3, 9). Родитель
// заменил K7 на R7a, хозяин писал под R7a; отзыв R7a пишет корень.
// BranchKeys::learn_revocation, услышав автора выше, выбрасывает записи и линии
// нижнего: блок хозяина под R7a становится чужим, а эпоха K7 — неотозванной, и
// блок вора старым ключом за линией снова «хозяина».
TEST(BranchKeysThefts, DISABLED_EscalationKeepsTheLowerEpochs) {
    Owner o("escalation");
    Agg   agg("escalation");
    const KeyPair k7 = o.keys.at(7);
    const Block b0 = o.data(7, k7);
    const KeyPair r7a = Crypto::generate_keypair();
    const Block rev1 = o.revoke(7, r7a.pub, 3, o.keys.at(3), Crypto::hash_block(b0));
    const Block b1 = o.data(7, r7a);
    const Block thief = signed_by(b1, k7);                       // K7 вора на #1, вне линии
    const KeyPair r7b = Crypto::generate_keypair();
    const Block rev2 = o.revoke(7, r7b.pub, 0, o.keys.at(0), Crypto::hash_block(b1));
    for (const Block& b : o.all_blocks()) agg.storage->add_block(b);

    BranchKeys keys;
    keys.learn_path(KeyPathCert::build(*o.storage, o.uid, 7));
    keys.learn_revocation(o.cert(rev1));
    ASSERT_EQ(keys.check(b1, agg.storage.get()), BranchKeys::Verdict::signed_by_branch);
    ASSERT_EQ(keys.check(thief, agg.storage.get()), BranchKeys::Verdict::revoked_key);
    keys.learn_revocation(o.cert(rev2));
    EXPECT_EQ(keys.check(b1, agg.storage.get()), BranchKeys::Verdict::signed_by_branch);
    EXPECT_EQ(keys.check(thief, agg.storage.get()), BranchKeys::Verdict::revoked_key);
    EXPECT_EQ(keys.whose(b1, *agg.storage), BranchKeys::Whose::owner);
    EXPECT_EQ(keys.whose(thief, *agg.storage), BranchKeys::Whose::not_owner);
}

// Украденный ключ выше стирает отзывы ниже навсегда. Родитель (3) заменил K7 на
// R7a; вор с ключом 1 пишет перехват 7 — автор выше, и BranchKeys выбрасывает
// записи родителя. Корень отзывает 1, перехват перестаёт считаться (blockchain.md §6.7 rule 10),
// но записи родителя уже не вернуть: агрегатор отвергнет их и при повторной
// выкладке («решил автор выше»). Validator в той же истории решает верно —
// он отбирает по правилу 10 до выбора автора.
TEST(BranchKeysThefts, DISABLED_AStolenHigherKeyDoesNotEraseLowerRecords) {
    Owner o("higher_key");
    Agg   agg("higher_key");
    const KeyPair k7 = o.keys.at(7);
    const Block b0 = o.data(7, k7);
    const KeyPair r7a = Crypto::generate_keypair();
    const Block rev1 = o.revoke(7, r7a.pub, 3, o.keys.at(3), Crypto::hash_block(b0));
    const Block b1 = o.data(7, r7a);
    const Block thief = signed_by(b1, k7);

    RevocationPayload rp{};                                      // ключ 1 украден
    rp.revoked_node_index = 7;
    rp.revoked_pubkey     = r7a.pub;
    rp.replacement_pubkey = Crypto::generate_keypair().pub;
    const auto tip1 = o.storage->branch_tip_index(o.uid, 1);
    const Block hijack = block_after({o.uid, 1, *tip1 + 1}, o.bc->branch_tip_hash(o.uid, 1), ++o.now,
                                     BlockType::REVOCATION, Serializer::encode(rp), o.keys.at(1));
    const Block g = o.revoke(1, std::nullopt, 0, o.keys.at(0), o.bc->branch_tip_hash(o.uid, 1));
    for (const Block& b : o.all_blocks()) agg.storage->add_block(b);

    for (const bool hijack_first : {false, true}) {
        SCOPED_TRACE(hijack_first);
        BranchKeys keys;
        keys.learn_path(KeyPathCert::build(*o.storage, o.uid, 7));
        if (hijack_first) keys.learn_revocation(cert_of(o, hijack));
        keys.learn_revocation(o.cert(rev1));
        if (!hijack_first) keys.learn_revocation(cert_of(o, hijack));
        keys.learn_revocation(o.cert(g));
        keys.learn_revocation(o.cert(rev1));                     // выложить ещё раз
        EXPECT_EQ(keys.check(b1, agg.storage.get()), BranchKeys::Verdict::signed_by_branch);
        EXPECT_EQ(keys.check(thief, agg.storage.get()), BranchKeys::Verdict::revoked_key);
    }
    // Своё хранилище хозяина: перехвата там нет, ветка в порядке.
    const auto s = o.validator->branch_revocation_status(o.uid, 7);
    for (auto b : s.blocks) EXPECT_EQ(b, BlockRevocationStatus::CLEAN);
}

// Сводка по стратегиям — без проверок, печатает таблицу: все сочетания краж в
// 1–3 раунда (399) × 4 стратегии. Долго, минуты полторы.
TEST(BranchKeysThefts, DISABLED_StrategyMatrix) {
    const auto all = combos(3);
    for (Strategy s : {Strategy::Root, Strategy::RootDefensive, Strategy::Parent,
                       Strategy::Escalate}) {
        int ok = 0, total = 0;
        for (const auto& c : all) {
            ++total;
            ok += play(c, s).why.empty();
        }
        std::printf("%-15s total %3d  aggregator decides for the owner %3d\n", strategy_name(s),
                    total, ok);
    }
}
