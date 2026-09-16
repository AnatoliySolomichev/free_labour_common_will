#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/fraud.h>
#include <blockchain/merge_session.h>
#include <blockchain/seal_manager.h>
#include <blockchain/serializer.h>
#include <blockchain/storage.h>
#include <blockchain/validator.h>
#include <records/carry.h>
#include <records/catalog.h>
#include <records/credit.h>
#include <records/json.h>
#include <records/codec.h>
#include <records/acceptance.h>
#include <records/goods_thread.h>
#include <records/draft.h>
#include <records/types.h>
#include <sync/dialogue_channel.h>
#include <sync/participant_cache.h>
#include <sync/snapshot_exchange.h>

#include <httplib.h>

#include <algorithm>
#include <tuple>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace blockchain;
using namespace records;

// ── Constants ─────────────────────────────────────────────────────────────────

// First leaf (depth 31): all user writes go to this branch by default.
static constexpr NodeIndex DEFAULT_LEAF = 0x7FFF'FFFFu;   // 2^31 − 1

// ── Hex utilities ─────────────────────────────────────────────────────────────

static std::string to_hex(const uint8_t* data, size_t len) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i)
        ss << std::setw(2) << static_cast<unsigned>(data[i]);
    return ss.str();
}

template<size_t N>
static std::string to_hex(const std::array<uint8_t, N>& a) {
    return to_hex(a.data(), N);
}

static bool from_hex(const std::string& s, uint8_t* out, size_t expected) {
    if (s.size() != expected * 2) return false;
    for (size_t i = 0; i < expected; ++i) {
        unsigned byte = 0;
        auto sub = s.substr(i * 2, 2);
        if (std::sscanf(sub.c_str(), "%02x", &byte) != 1) return false;
        out[i] = static_cast<uint8_t>(byte);
    }
    return true;
}

// First `n` bytes as hex + "..." for compact display.
static std::string short_hex(const std::array<uint8_t, 32>& a, size_t n = 8) {
    return to_hex(a.data(), n) + "...";
}

static std::vector<uint8_t> from_hex_vec(const std::string& s) {
    if (s.size() % 2 != 0)
        throw std::runtime_error("odd-length hex string");
    std::vector<uint8_t> result(s.size() / 2);
    if (!result.empty() && !from_hex(s, result.data(), result.size()))
        throw std::runtime_error("invalid hex characters");
    return result;
}

// ── Key store ─────────────────────────────────────────────────────────────────
// Each .key file: 32-byte PublicKey || 64-byte SecretKey = 96 bytes.

static fs::path key_path(const fs::path& data_dir, NodeIndex idx) {
    return data_dir / "keys" / (std::to_string(idx) + ".key");
}

static void save_keypair(const fs::path& data_dir, NodeIndex idx, const KeyPair& kp) {
    fs::create_directories(data_dir / "keys");
    std::ofstream f(key_path(data_dir, idx), std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write key file for node " + std::to_string(idx));
    f.write(reinterpret_cast<const char*>(kp.pub.bytes.data()), 32);
    f.write(reinterpret_cast<const char*>(kp.sec.bytes.data()), 64);
}

static KeyPair load_keypair(const fs::path& data_dir, NodeIndex idx) {
    std::ifstream f(key_path(data_dir, idx), std::ios::binary);
    if (!f) throw std::runtime_error(
        "key not found for node " + std::to_string(idx) +
        " — run: bc identity create");
    KeyPair kp{};
    f.read(reinterpret_cast<char*>(kp.pub.bytes.data()), 32);
    f.read(reinterpret_cast<char*>(kp.sec.bytes.data()), 64);
    if (!f) throw std::runtime_error("key file truncated for node " + std::to_string(idx));
    return kp;
}

static UserId load_user_id(const fs::path& data_dir) {
    return load_keypair(data_dir, 0).pub;
}

// ── Argument helpers ──────────────────────────────────────────────────────────

// All flags that consume a following value (used by get_positionals to skip them).
static const std::set<std::string> VALUE_FLAGS = {
    "--data-dir", "--leaf", "--tag", "--kind", "--part",
    "--value", "--chain",
    "--title", "--action", "--hours", "--start", "--input", "--output",
    "--work", "--like", "--quality", "--hours-raw", "--labor-units",
    "--peer-tip", "--peer-snapshot", "--draft", "--co-sig", "--mode", "--user",
    "--idx", "--depth", "--proof", "--merkle-root", "--target", "--leaf-hash",
    "--reason", "--peer", "--via", "--timeout",
    "--to", "--units", "--origin", "--pledge", "--executor", "--expires",
    "--acceptance", "--coef", "--k", "--with", "--to-node",
    "--node", "--ancestor", "--since", "--out", "--cert", "--hex",
    "--skill", "--need", "--search",
    "--tool", "--cost", "--life", "--serial", "--desc", "--note", "--paid",
    "--material", "--unit", "--qty",
    "--buyback-to", "--buyback-days", "--buyback-until", "--buyback-of",
};

// True when a standalone flag (no value) is present.
static bool flag_present(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == flag) return true;
    return false;
}

// Returns all non-flag arguments in order.
static std::vector<std::string> get_positionals(int argc, char** argv) {
    std::vector<std::string> result;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (VALUE_FLAGS.count(a)) { ++i; continue; }  // skip --flag VALUE
        if (a.rfind("--", 0) == 0) continue;           // skip standalone flags
        result.push_back(a);
    }
    return result;
}

// First value for --flag, or default.
static std::string flag_val(int argc, char** argv,
                             const std::string& flag,
                             const std::string& def = "") {
    for (int i = 1; i < argc - 1; ++i)
        if (std::string(argv[i]) == flag) return argv[i + 1];
    return def;
}

// All values for repeated --flag v1 --flag v2 → ["v1", "v2"].
static std::vector<std::string> flag_all(int argc, char** argv,
                                          const std::string& flag) {
    std::vector<std::string> result;
    for (int i = 1; i < argc - 1; ++i)
        if (std::string(argv[i]) == flag) result.push_back(argv[i + 1]);
    return result;
}

// ── Leaf index parser ─────────────────────────────────────────────────────────

static NodeIndex parse_leaf_index(int argc, char** argv) {
    const auto s = flag_val(argc, argv, "--leaf");
    if (s.empty()) return DEFAULT_LEAF;
    unsigned long long v = 0;
    try { v = std::stoull(s, nullptr, 0); }
    catch (...) { throw std::runtime_error("--leaf: invalid index: " + s); }
    // Branches may grow from any node of the tree (blockchain.md §3.2 v0.7).
    if (v > 0xFFFF'FFFEu || !is_valid_node(static_cast<NodeIndex>(v)))
        throw std::runtime_error(
            "--leaf " + s + " is not a valid node index (0..0xFFFFFFFE)");
    return static_cast<NodeIndex>(v);
}

// ── Ref parser ("<chain_64hex>/<hash_64hex>") ─────────────────────────────────

static Ref parse_ref(const std::string& s) {
    const auto slash = s.find('/');
    if (slash == std::string::npos)
        throw std::runtime_error("expected <chain_hex>/<hash_hex>, got: " + s);
    Ref ref{};
    if (!from_hex(s.substr(0, slash), ref.chain.data(), 32) ||
        !from_hex(s.substr(slash + 1), ref.hash.data(), 32))
        throw std::runtime_error("invalid hex in ref: " + s);
    return ref;
}

// Full path to a block: <chain_hex>/<node>/<block>/<hash_hex>. The path is what a
// person can walk by hand; the hash checks that the block there is the one meant.
static BlockPath parse_block_path(const std::string& s) {
    std::vector<std::string> part;
    std::size_t start = 0;
    for (;;) {
        const auto slash = s.find('/', start);
        part.push_back(s.substr(start, slash == std::string::npos ? std::string::npos
                                                                  : slash - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    if (part.size() != 4)
        throw std::runtime_error("ожидался путь <цепь>/<узел>/<блок>/<хеш>, получено: " + s);
    BlockPath p{};
    if (!from_hex(part[0], p.chain.data(), 32) || !from_hex(part[3], p.hash.data(), 32))
        throw std::runtime_error("неверный hex в пути: " + s);
    try {
        const unsigned long node  = std::stoul(part[1]);
        const unsigned long block = std::stoul(part[2]);
        if (node > 0xFFFFFFFFul || block > 0xFFFFFFFFul) throw std::out_of_range("index");
        p.node  = static_cast<uint32_t>(node);
        p.block = static_cast<uint32_t>(block);
    } catch (const std::exception&) {
        throw std::runtime_error("неверный номер узла или блока в пути: " + s);
    }
    return p;
}

// ── ResourceQty parser ("name:qty:unit") ─────────────────────────────────────

static ResourceQty parse_rq(const std::string& s) {
    const auto p1 = s.find(':');
    if (p1 == std::string::npos) throw std::runtime_error("bad resource format: " + s);
    const auto p2 = s.find(':', p1 + 1);
    if (p2 == std::string::npos) throw std::runtime_error("bad resource format: " + s);
    return { s.substr(0, p1), std::stod(s.substr(p1 + 1, p2 - p1 - 1)), s.substr(p2 + 1) };
}

// A comma-separated list: «R1,R2,R3»; empty items are dropped.
static std::vector<std::string> split_list(const std::string& s) {
    std::vector<std::string> out;
    for (std::size_t from = 0;;) {
        const auto comma = s.find(',', from);
        std::string item = s.substr(from, comma == std::string::npos ? std::string::npos
                                                                     : comma - from);
        if (!item.empty()) out.push_back(std::move(item));
        if (comma == std::string::npos) break;
        from = comma + 1;
    }
    return out;
}

// --output ИМЯ:КОЛИЧЕСТВО:ЕДИНИЦА[:ДОЛЯ[:ЦЕНА[:НОМЕРА]]] — a lot (records.md §9.4
// v6): the share of the work's hours when one work yields several outputs; the
// price tag — labor-hours for the whole lot, or k=K to have it computed from
// the axis prices of the production day; the unit numbers, comma separated,
// for a lot counted piece by piece. An empty field is absent:
// «хлеб:100:шт::k=1.2» — a tag and no share; «табурет:3:шт::k=1:Т-1,Т-2,Т-3».
struct OutputSpec {
    ResourceQty           rq;
    std::optional<double> k;   // the tag still to be computed from k
};

static OutputSpec parse_output(const std::string& s) {
    std::vector<std::string> parts;
    for (std::size_t from = 0;;) {
        const auto colon = s.find(':', from);
        parts.push_back(s.substr(from, colon == std::string::npos ? std::string::npos
                                                                  : colon - from));
        if (colon == std::string::npos) break;
        from = colon + 1;
    }
    if (parts.size() < 3 || parts.size() > 7)
        throw std::runtime_error("--output ждёт ИМЯ:КОЛИЧЕСТВО:ЕДИНИЦА[:ДОЛЯ[:ЦЕНА[:НОМЕРА[:СРОК]]]], "
                                 "получено: " + s);
    OutputSpec spec{ResourceQty{parts[0], std::stod(parts[1]), parts[2]}, std::nullopt};
    if (parts.size() > 3 && !parts[3].empty()) spec.rq.share = std::stod(parts[3]);
    if (parts.size() > 4 && !parts[4].empty()) {
        if (parts[4].rfind("k=", 0) == 0) spec.k = std::stod(parts[4].substr(2));
        else                              spec.rq.price = std::stod(parts[4]);
    }
    if (parts.size() > 5) spec.rq.units = split_list(parts[5]);
    // Срок службы, дни (records.md §9.4 v7): по нему считается износ при аренде.
    if (parts.size() > 6 && !parts[6].empty()) spec.rq.life = std::stod(parts[6]);
    if (!spec.rq.units.empty() && spec.rq.qty != static_cast<double>(spec.rq.units.size()))
        throw std::runtime_error("у партии с номерами количество — число номеров: " + s);
    return spec;
}

// A quantity as people write it: 3, 2.5 — not 3.000000.
static std::string qty_str(double v) {
    std::ostringstream ss;
    ss << v;
    return ss.str();
}

// ── Record summary for list ───────────────────────────────────────────────────

static std::string record_summary(const Record& rec) {
    return std::visit([](const auto& r) -> std::string {
        using T = std::decay_t<decltype(r)>;
        std::ostringstream ss;
        if constexpr (std::is_same_v<T, Concept>) {
            ss << "[Concept]     \"" << r.text << "\"";
            if (!r.tags.empty()) {
                ss << "  tags:[";
                for (size_t i = 0; i < r.tags.size(); ++i) { if (i) ss << ","; ss << r.tags[i]; }
                ss << "]";
            }
        } else if constexpr (std::is_same_v<T, ConceptLink>) {
            ss << "[ConceptLink] " << short_hex(r.from.hash)
               << " -[" << r.kind << "]-> " << short_hex(r.to.hash);
        } else if constexpr (std::is_same_v<T, Composite>) {
            ss << "[Composite]   \"" << r.title << "\"  parts:" << r.parts.size();
        } else if constexpr (std::is_same_v<T, Copy>) {
            ss << "[Copy]        src:"
               << short_hex(r.source.chain) << "/" << short_hex(r.source.hash);
        } else if constexpr (std::is_same_v<T, Reaction>) {
            const int v = static_cast<int>(r.value);
            ss << "[Reaction]    target:"
               << short_hex(r.target.chain) << "/" << short_hex(r.target.hash)
               << "  value:" << (v >= 0 ? "+" : "") << v;
        } else if constexpr (std::is_same_v<T, WorkRecord>) {
            ss << "[WorkRecord]  \"" << r.action << "\"  " << r.hours << "h";
            if (!r.title.empty()) ss << "  [" << r.title << "]";
            if (!r.inputs.empty())  ss << "  in:"  << r.inputs.size();
            if (!r.outputs.empty()) ss << "  out:" << r.outputs.size();
            if (!r.carry.empty()) {
                double carried = 0;
                for (const auto& ce : r.carry) carried += ce.carried;
                ss << "  carry:" << r.carry.size() << " (" << carried << "h)";
            }
            if (r.customer) ss << "  for:" << short_hex(*r.customer);
            if (r.thread) ss << "  thread#" << r.thread->seq
                             << (r.thread->prev.size() > 1 ? " merge" : "");
            if (!r.axes.empty()) ss << "  axes:" << r.axes.size();
        } else if constexpr (std::is_same_v<T, records::Release>) {
            ss << "[Release]     work:" << short_hex(r.work.hash)
               << "  to:" << short_hex(r.to) << "  expires:" << r.expires;
            if (r.lot)
                ss << "  lot:" << r.lot->out << " qty:" << r.lot->qty
                   << " #" << r.lot->seq << " after:" << r.lot->after;
            if (r.holding) ss << "  resale of:" << short_hex(r.holding->hash);
            if (r.addressed)
                ss << "  addressed:" << short_hex(r.addressed->to) << " until:"
                   << r.addressed->until << (r.addressed->wear ? " rent" : "");
        } else if constexpr (std::is_same_v<T, records::WriteOff>) {
            ss << "[WriteOff]    work:" << short_hex(r.work.hash) << "  lot:" << r.lot.out
               << " qty:" << r.lot.qty << " #" << r.lot.seq << " after:" << r.lot.after
               << "  \"" << r.reason << "\"";
            if (r.holding) ss << "  bought:" << short_hex(r.holding->hash);
        } else if constexpr (std::is_same_v<T, records::Tool>) {
            ss << "[Tool]        \"" << r.name << "\"  " << r.cost
               << "h  life:" << r.life << "h  [" << r.basis << "]";
            if (r.origin) ss << "  origin:" << short_hex(r.origin->hash);
        } else if constexpr (std::is_same_v<T, records::Material>) {
            ss << "[Material]    \"" << r.name << "\"  " << r.qty << " " << r.unit
               << "  " << r.cost << "h  [" << r.basis << "]";
            if (r.origin) ss << "  origin:" << short_hex(r.origin->hash);
        } else if constexpr (std::is_same_v<T, records::Transfer>) {
            double total = 0;
            for (const auto& o : r.origins) total += o.units;
            ss << "[Transfer]    to:" << short_hex(r.to)
               << "  " << total << "h  (" << r.origins.size() << " portion(s))";
            if (r.accept) {   // v5: the deal — acceptance and payment in one record
                ss << "  accept work:" << short_hex(r.accept->work.hash)
                   << " " << r.accept->labor_units << " labor-h";
                if (r.accept->carried_units)
                    ss << " +carry:" << *r.accept->carried_units << "h";
                ss << " axes:" << r.accept->axes.size();
                if (r.accept->like) ss << " like:" << short_hex(r.accept->like->hash);
                if (r.accept->buyback)
                    ss << " return→" << short_hex(r.accept->buyback->to)
                       << (r.accept->buyback->wear ? " rent" : "");
                ss << " quality:" << r.accept->quality;
            }
            if (r.emission)
                ss << "  link#" << r.emission->seq
                   << " debt:" << r.emission->debt_after;
        } else if constexpr (std::is_same_v<T, records::Redemption>) {
            ss << "[Redemption]  transfer:" << short_hex(r.transfer.hash)
               << "  +" << r.units << "h  link#" << r.link.seq
               << "  debt:" << r.link.debt_after;
        } else if constexpr (std::is_same_v<T, Pledge>) {
            ss << "[Pledge]      target:" << short_hex(r.target.hash)
               << "  " << r.units << "h";
            if (r.executor) ss << "  exec:" << short_hex(*r.executor);
            if (r.expires)  ss << "  expires:" << *r.expires;
        } else if constexpr (std::is_same_v<T, PledgeRevoke>) {
            ss << "[PledgeRevoke] pledge:" << short_hex(r.pledge.hash);
        }
        return ss.str();
    }, rec);
}

// ── Open storage helper ───────────────────────────────────────────────────────

struct Context {
    fs::path    data_dir;
    LmdbStorage storage;
    Validator   validator;
    Blockchain  bc;
    UserId      user_id;
    KeyPair     working_kp;
    NodeIndex   leaf;

    Context(const fs::path& dir, NodeIndex leaf_index)
        : data_dir  (dir)
        , storage   (dir / "db")
        , validator (storage)
        , bc        (storage, validator)
        , user_id   (load_user_id(dir))
        , working_kp(load_keypair(dir, leaf_index))
        , leaf      (leaf_index)
    {}

    // Local branches that actually hold blocks (key files name candidate
    // nodes; a branch exists once its tip index does).
    std::vector<NodeIndex> local_branches() {
        std::vector<NodeIndex> out;
        const auto keys_dir = data_dir / "keys";
        if (!fs::exists(keys_dir)) return out;
        for (const auto& entry : fs::directory_iterator(keys_dir)) {
            NodeIndex idx = 0;
            try { idx = static_cast<NodeIndex>(std::stoul(entry.path().stem().string())); }
            catch (...) { continue; }
            if (storage.branch_tip_index(user_id, idx).has_value())
                out.push_back(idx);
        }
        return out;
    }
};

// ── Revocation freshness (blockchain.md §6.7 rule 11; sync.md §10.3) ──────────

struct RevFetchResult {
    bool                reachable = false;
    std::size_t         imported  = 0;
    std::size_t         rejected  = 0;
    std::set<NodeIndex> touched;   // revoked nodes seen in imported certificates
};

// Pull the chain's revocation certificates from the warehouse, verify each
// autonomously (the warehouse is untrusted) and import the good ones: path
// nodes + the REVOCATION block (external) feed the local revocation index.
static RevFetchResult fetch_import_revocations(LmdbStorage& storage,
                                               const UserId& chain,
                                               const std::string& via) {
    RevFetchResult r{};
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get("/revocations/" + to_hex(chain.bytes));
    if (!res || res->status != 200) return r;
    r.reachable = true;

    // CBOR array(bstr) — minimal parse mirroring the warehouse encoder.
    const std::string& body = res->body;
    std::size_t bpos = 0;
    auto head = [&](uint8_t expect_major) -> long long {
        if (bpos >= body.size()) return -1;
        const uint8_t b = static_cast<uint8_t>(body[bpos++]);
        if ((b >> 5) != expect_major) return -1;
        const uint8_t ai = b & 0x1F;
        if (ai < 24) return ai;
        int extra = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : -1;
        if (extra < 0 || bpos + extra > body.size()) return -1;
        long long v = 0;
        for (int i = 0; i < extra; ++i)
            v = (v << 8) | static_cast<uint8_t>(body[bpos++]);
        return v;
    };

    const long long n = head(4);
    for (long long i = 0; i < n; ++i) {
        const long long len = head(2);
        if (len < 0 || bpos + len > body.size()) break;
        const auto* p = reinterpret_cast<const uint8_t*>(body.data()) + bpos;
        bpos += static_cast<std::size_t>(len);
        try {
            const auto cert = Serializer::decode_revocation_cert(
                p, static_cast<size_t>(len));
            RevocationCert::verify(cert);
            if (!(cert.block.address.user_id == chain))
                throw RevocationError("certificate belongs to another chain");

            for (const auto& node : cert.path)
                if (!storage.has_node(chain, node.index))
                    storage.put_node(chain, node);
            if (!storage.has_block(cert.block.address) &&
                !storage.has_external_block(cert.block.address))
                storage.put_external_block(cert.block);

            r.touched.insert(RevocationCert::payload(cert).revoked_node_index);
            ++r.imported;
        } catch (const std::exception&) { ++r.rejected; }
    }
    return r;
}

// ── Write helper: encode + append DATA block ──────────────────────────────────

static void upload_block(const std::string& via, const Block& block);

// Writes the record and, when --via is given, uploads the block to an
// aggregator so others can 'bc fetch' it.
static int cmd_write(const fs::path& data_dir, int argc, char** argv,
                     const Record& rec) {
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    const auto payload = Codec::encode(rec);
    const auto now     = static_cast<Timestamp>(std::time(nullptr));
    const auto block   = ctx.bc.append_data_block(
        ctx.user_id, leaf, payload, ctx.working_kp, now);
    const auto hash    = Crypto::hash_block(block);
    std::cout << "block #" << block.address.block_index
              << "  hash: " << to_hex(hash.bytes) << "\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

static Block append_record(Context& ctx, const Record& rec) {
    const auto payload = Codec::encode(rec);
    const auto now     = static_cast<Timestamp>(std::time(nullptr));
    return ctx.bc.append_data_block(ctx.user_id, ctx.leaf, payload,
                                    ctx.working_kp, now);
}

// Locate a previously fetched foreign block by its hash (external store is
// keyed by address, so this scans).
static std::optional<Block> find_external_by_hash(
        Context& ctx, const std::array<uint8_t, 32>& hash) {
    std::optional<Block> found;
    ctx.storage.for_each_external_block([&](const Block& b) {
        if (Crypto::hash_block(b).bytes == hash) { found = b; return false; }
        return true;
    });
    return found;
}

// Fetch a block by ref from an aggregator and verify it really is the
// referenced one (hash and owning chain must match the ref).
static Block fetch_block_from(const std::string& via, const Ref& src) {
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get("/sync/block/" + to_hex(src.hash.data(), 32));
    if (!res || res->status != 200)
        throw std::runtime_error("block not found on " + via);
    const Block block = Serializer::decode_block(
        reinterpret_cast<const uint8_t*>(res->body.data()), res->body.size());
    if (Crypto::hash_block(block).bytes != src.hash)
        throw std::runtime_error("aggregator returned a block with a different hash");
    if (block.address.user_id.bytes != src.chain)
        throw std::runtime_error("block belongs to a different chain than the ref says");
    return block;
}

// A record by Ref: the own chain, then the local external store, then the
// aggregator (verified and cached). nullopt on any gap.
static std::optional<Record> load_record(Context& ctx, const std::string& via,
                                         const Ref& ref) {
    std::optional<Block> b;
    if (ref.chain == ctx.user_id.bytes) {
        for (const NodeIndex node : ctx.local_branches()) {
            std::vector<Block> blocks;
            try { blocks = ctx.bc.get_branch(ctx.user_id, node); }
            catch (const BlockchainError&) { continue; }
            for (const auto& blk : blocks)
                if (Crypto::hash_block(blk).bytes == ref.hash) { b = blk; break; }
            if (b) break;
        }
    }
    if (!b) b = find_external_by_hash(ctx, ref.hash);
    if (!b && !via.empty()) {
        try {
            b = fetch_block_from(via, ref);
            if (!ctx.storage.has_external_block(b->address))
                ctx.storage.put_external_block(*b);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    if (!b || b->type != BlockType::DATA) return std::nullopt;
    try { return Codec::decode(b->payload.data(), b->payload.size()); }
    catch (const CodecError&) { return std::nullopt; }
}

// Network prices of an hour of each axis (records.md §11.2 v5) from an
// /economy/rates response: axis hash → price. Empty on any gap — the caller
// falls back and says so.
static std::map<std::array<uint8_t, 32>, double> axis_prices_at(const std::string& via,
                                                                const std::string& path) {
    std::map<std::array<uint8_t, 32>, double> out;
    if (via.empty()) return out;
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get(path);
    if (!res || res->status != 200) return out;
    try {
        const auto root = records::json::Reader(res->body).read();
        const auto* axes = root.is_object() ? records::json::find(root.object, "axes")
                                            : nullptr;
        if (!axes || !axes->is_array()) return out;
        for (const auto& e : axes->array) {
            if (!e.is_object()) continue;
            const auto* ax = records::json::find(e.object, "axis");
            const auto* pr = records::json::find(e.object, "price");
            if (!ax || !ax->is_string() || !pr || !pr->is_number()) continue;
            out[parse_block_path(ax->string).hash] = pr->number;
        }
    } catch (...) {
        out.clear();
    }
    return out;
}

// Today's prices.
static std::map<std::array<uint8_t, 32>, double> lookup_axis_prices(const std::string& via) {
    return axis_prices_at(via, "/economy/rates");
}

// The prices a product made on day `ts` is valued at (ИР-023, вопрос 2): the
// DailyAggregate the aggregator published that day. Empty when none was — the
// caller falls back to today's prices and says so.
static std::map<std::array<uint8_t, 32>, double> lookup_axis_prices_on(const std::string& via,
                                                                       int64_t ts) {
    return axis_prices_at(via, "/economy/rates?date=" + std::to_string(ts));
}

// Best-effort normalizer provenance for an acceptance (economy.md §2б): the W the
// aggregator publishes today + which aggregate it is (chain + date). Failure to
// reach it is fine — the acceptance is signed anyway, just without norm.
static std::optional<AcceptanceNorm> fetch_norm(const std::string& via) {
    if (via.empty()) return std::nullopt;
    httplib::Client cli(via);
    cli.set_connection_timeout(5);
    const auto res = cli.Get("/economy/rates");
    if (!res || res->status != 200) return std::nullopt;
    try {
        const auto root = records::json::Reader(res->body).read();
        if (!root.is_object()) return std::nullopt;
        const auto* w  = records::json::find(root.object, "W");
        const auto* ch = records::json::find(root.object, "chain");
        const auto* dt = records::json::find(root.object, "date");
        if (!w || !w->is_number() || !ch || !ch->is_string() ||
            !dt || !dt->is_number())
            return std::nullopt;
        AcceptanceNorm n{};
        n.W    = w->number;
        n.date = static_cast<int64_t>(dt->number);
        if (!from_hex(ch->string, n.agg.data(), 32)) return std::nullopt;
        return n;
    } catch (...) { return std::nullopt; }
}

// ── Carry: перенос стоимости средств производства (ИР-011, records.md §9.4) ──

// The fields the carry logic needs from a Tool or Material record.
struct AssetInfo {
    std::string name;
    double      cost        = 0;
    double      capacity    = 0;  // Tool.life (hours) / Material.qty (units)
    std::string basis;            // "paid" | "est"
    bool        is_material = false;
    std::string unit;             // material only
};

static std::optional<AssetInfo> asset_info(const Record& rec) {
    if (const auto* t = std::get_if<records::Tool>(&rec))
        return AssetInfo{t->name, t->cost, t->life, t->basis, false, ""};
    if (const auto* m = std::get_if<records::Material>(&rec))
        return AssetInfo{m->name, m->cost, m->qty, m->basis, true, m->unit};
    return std::nullopt;
}

// A means-of-production record in the own chain, found by block-hash prefix
// (empty prefix = all). Own chain only: you carry cost off your own assets.
struct OwnAsset {
    Ref       ref;
    AssetInfo info;
};

static std::vector<OwnAsset> find_own_assets(Context& ctx,
                                             const std::string& hash_prefix) {
    std::vector<OwnAsset> out;
    for (const NodeIndex branch : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, branch); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }
            const auto info = asset_info(rec);
            if (!info) continue;
            const auto hash = Crypto::hash_block(b).bytes;
            if (!hash_prefix.empty() &&
                to_hex(hash).compare(0, hash_prefix.size(), hash_prefix) != 0)
                continue;
            out.push_back({Ref{ctx.user_id.bytes, hash}, *info});
        }
    }
    return out;
}

// The carry-thread tip of one asset across ALL local branches (records.md
// §9.4): the thread is one per asset for the whole chain — prev is a Ref and
// crosses branches; scanning every branch is what keeps it linear.
struct CarryThreadState {
    uint64_t           next_seq  = 0;
    double             collected = 0;
    std::optional<Ref> tip;   // block carrying the newest link
};

static CarryThreadState carry_thread_state(Context& ctx, const Ref& asset) {
    CarryThreadState st{};
    bool found = false;
    for (const NodeIndex branch : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, branch); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }
            const auto* wr = std::get_if<WorkRecord>(&rec);
            if (!wr) continue;
            for (const auto& ce : wr->carry) {
                if (!(ce.src == asset)) continue;
                if (!found || ce.seq + 1 > st.next_seq) {
                    found        = true;
                    st.next_seq  = ce.seq + 1;
                    st.collected = ce.after;
                    st.tip       = Ref{ctx.user_id.bytes,
                                       Crypto::hash_block(b).bytes};
                }
            }
        }
    }
    return st;
}

// Parse repeatable --tool "HASH_PREFIX:USED" specs and attach carry links to
// the work record. The CLI runs the thread itself — the exact pattern of the
// emission thread in bc pay (economy.md §4.3).
static void attach_carry(Context& ctx, WorkRecord& wr,
                         const std::vector<std::string>& specs) {
    for (const auto& s : specs) {
        const auto colon = s.rfind(':');
        if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size())
            throw std::runtime_error(
                "--tool ждёт ПРЕФИКС_ХЕША:ЧАСЫ (для партии — :КОЛИЧЕСТВО), получено: " + s);
        const std::string prefix = s.substr(0, colon);
        const double      used   = std::stod(s.substr(colon + 1));
        if (used <= 0) throw std::runtime_error("--tool: часы/количество должны быть > 0");

        const auto matches = find_own_assets(ctx, prefix);
        if (matches.empty())
            throw std::runtime_error("средство производства '" + prefix +
                                     "' не найдено в своей цепи — сперва bc tool add");
        if (matches.size() > 1)
            throw std::runtime_error("префикс '" + prefix + "' неоднозначен (" +
                                     std::to_string(matches.size()) + " записи)");
        const auto& a  = matches[0];
        const auto  st = carry_thread_state(ctx, a.ref);

        // A batch can't yield more than its size; a tool outliving its rated
        // life is legitimate (free machine-hours, ИР-011), so warn for the
        // material only.
        const double used_before = a.info.capacity > 0
            ? st.collected / a.info.cost * a.info.capacity : 0;
        if (a.info.is_material && used_before + used > a.info.capacity + 1e-9)
            std::cerr << "  предупреждение: списано больше размера партии ("
                      << a.info.name << ", партия " << a.info.capacity << " "
                      << a.info.unit << ") — заведите новую партию для остатка\n";

        records::CarryEntry ce{};
        ce.src     = a.ref;
        ce.used    = used;
        ce.carried = records::carry_step(a.info.cost, a.info.capacity,
                                         used, st.collected);
        ce.seq     = st.next_seq;
        ce.prev    = st.tip;
        ce.after   = st.collected + ce.carried;
        wr.carry.push_back(ce);

        std::cerr << "  перенос: " << ce.carried << "ч  (" << a.info.name
                  << ", собрано " << ce.after << "/" << a.info.cost << "ч"
                  << (a.info.basis == "est" ? ", оценка владельца" : "") << ")";
        if (ce.carried <= 1e-9)
            std::cerr << (a.info.is_material
                          ? "  — партия исчерпана, заведите новую (records.md §10.1)"
                          : "  — стоимость возвращена, работает бесплатно (records.md §9.4)");
        std::cerr << "\n";
    }
}

// The goods thread (records.md §9.4 v4, ИР-023): a work goes into the chain's
// one thread — after every link this device can see — and claims hours of its
// own. Without --start the work is taken to have just ended; if those hours are
// already claimed by another work of the chain, it goes into the latest free
// window before them — never into the future, whose hours belong to work not
// yet done. The same hours are never sold twice: an explicit --start that
// crosses another work is refused.
static void place_in_thread(Context& ctx, WorkRecord& wr, bool explicit_start) {
    std::vector<records::ObservedWork>          seen;   // own works, every local branch
    std::map<std::array<uint8_t, 32>, uint64_t> seq_of;
    for (const NodeIndex branch : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, branch); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }
            const auto* w = std::get_if<WorkRecord>(&rec);
            if (!w) continue;
            const auto h = Crypto::hash_block(b).bytes;
            seen.push_back({h, records::labor_interval(*w), w->thread});
            if (w->thread) seq_of[h] = w->thread->seq;
        }
    }

    const auto crossing = [&]() -> const records::ObservedWork* {
        const auto iv = records::labor_interval(wr);
        for (const auto& o : seen)
            if (records::intervals_cross(iv, o.interval)) return &o;
        return nullptr;
    };
    // Будущих часов не заявляют (records.md §9.4 v5, решение участника 2026-09-16):
    // записывают только законченную работу, иначе она заняла бы место той, что ещё
    // впереди.
    if (records::labor_interval(wr).second > static_cast<int64_t>(std::time(nullptr)))
        throw std::runtime_error(
            "работа кончается в будущем — записывают только законченную работу "
            "(records.md §9.4). Без --start работа считается только что законченной");
    // Клиент ничего не угадывает (решение участника 2026-09-16): часы заняты —
    // отказ, а не сдвиг. Время работы называет человек, и только свободное.
    if (const auto* o = crossing()) {
        const int64_t length = records::labor_interval(wr).second - wr.start_ts;
        const auto busy = [&](int64_t from) -> const records::ObservedWork* {
            for (const auto& s : seen)
                if (records::intervals_cross({from, from + length}, s.interval)) return &s;
            return nullptr;
        };
        int64_t free_start = wr.start_ts;   // самое позднее свободное окно перед занятыми
        while (const auto* c = busy(free_start)) free_start = c->interval.first - length;
        throw std::runtime_error(
            "эти часы уже заняты работой " + short_hex(o->hash) + " — одни и те же часы не "
            "продаются дважды (records.md §9.4). Назовите время работы сами: ближайшее "
            "свободное окно перед занятыми — --start " + std::to_string(free_start));
    }

    const auto th = records::check_labor_thread(ctx.user_id.bytes, seen);
    records::ThreadLink link{};
    for (const auto& tip : th.tips) {
        link.prev.push_back(Ref{ctx.user_id.bytes, tip});
        link.seq = std::max(link.seq, seq_of[tip] + 1);
    }
    std::sort(link.prev.begin(), link.prev.end(), [](const Ref& x, const Ref& y) {
        return std::tie(x.chain, x.hash) < std::tie(y.chain, y.hash);
    });
    wr.thread = link;
    std::cerr << "нить труда: звено #" << link.seq;
    if (link.prev.size() > 1) std::cerr << " — сводит " << link.prev.size() << " ветки";
    std::cerr << "\n";
}

// --axis-share ПУТЬ_ОСИ=ДОЛЯ, repeatable: the worker's proposed breakdown of the
// hours by axis (records.md §9.4 v4). The last axis takes the remainder, so the
// shares add up to 1 exactly, not up to rounding.
static std::vector<records::AxisShare> parse_axis_shares(const std::vector<std::string>& specs) {
    std::vector<records::AxisShare> out;
    double sum = 0.0;
    for (const auto& p : specs) {
        const auto eq = p.rfind('=');
        if (eq == std::string::npos || eq == 0)
            throw std::runtime_error("--axis-share ждёт ПУТЬ_ОСИ=ДОЛЯ (путь: цепь/узел/блок/хеш): " + p);
        records::AxisShare x{};
        x.axis  = parse_block_path(p.substr(0, eq));
        x.share = std::stod(p.substr(eq + 1));
        sum += x.share;
        out.push_back(x);
    }
    if (out.empty()) return out;
    if (std::abs(sum - 1.0) > 1e-6)
        throw std::runtime_error("--axis-share в сумме даёт " + std::to_string(sum) +
                                 ", а должно 1.0");
    double left = 1.0;
    for (std::size_t i = 0; i + 1 < out.size(); ++i) left -= out[i].share;
    out.back().share = left;
    std::sort(out.begin(), out.end(), [](const records::AxisShare& a, const records::AxisShare& b) {
        return a.axis.hash < b.axis.hash;
    });
    return out;
}

// bc block stub [--leaf L]
// Appends an empty stub DATA block: bootstrap for merge (blockchain.md §6.4) or time anchor (blockchain.md §5.4).
static int cmd_block_stub(const fs::path& data_dir, int argc, char** argv) {
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    const auto now   = static_cast<Timestamp>(std::time(nullptr));
    const auto block = ctx.bc.append_stub_block(ctx.user_id, leaf, ctx.working_kp, now);
    const auto hash  = Crypto::hash_block(block);
    std::cout << "stub block #" << block.address.block_index
              << "  hash: " << to_hex(hash.bytes) << "\n";
    return 0;
}

// ── Merge state persistence ───────────────────────────────────────────────────
// Saved between 'merge create' and 'merge finalize' in data_dir/merge/<leaf>.state.
// Format (all LE): partner_pubkey(32) | draft_block_index(4)
//                  | tip_len(4)      | peer_tip_cbor(N)
//                  | own_tip_len(4)  | own_tip_cbor(N)     ┐ pre-merge data for
//                  | own_snap_len(4) | own_snap_cbor(N)    │ the participant
//                  | peer_snap_len(4)| peer_snap_cbor(N)   ┘ cache (sync.md §5.2)
// The three trailing blobs may be absent (older state files) — the cache fill
// is then skipped.

struct MergeState {
    PublicKey            partner_pubkey;
    BlockIndex           draft_block_index;
    std::vector<uint8_t> peer_tip_cbor;
    std::vector<uint8_t> own_tip_cbor;    // BranchTipInfo as of 'merge create'
    std::vector<uint8_t> own_snap_cbor;   // own PRE-merge MergeSnapshot
    std::vector<uint8_t> peer_snap_cbor;  // partner's PRE-merge MergeSnapshot
};

static fs::path merge_state_path(const fs::path& data_dir, NodeIndex leaf) {
    return data_dir / "merge" / (std::to_string(leaf) + ".state");
}

static void save_merge_state(const fs::path& data_dir, NodeIndex leaf,
                              const MergeState& state) {
    const auto path = merge_state_path(data_dir, leaf);
    fs::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) throw std::runtime_error("cannot write merge state: " + path.string());

    f.write(reinterpret_cast<const char*>(state.partner_pubkey.bytes.data()), 32);

    auto le32 = [](uint32_t v, std::ofstream& out) {
        uint8_t b[4] = { uint8_t(v), uint8_t(v>>8), uint8_t(v>>16), uint8_t(v>>24) };
        out.write(reinterpret_cast<const char*>(b), 4);
    };
    auto blob = [&le32](const std::vector<uint8_t>& bytes, std::ofstream& out) {
        le32(static_cast<uint32_t>(bytes.size()), out);
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
    };
    le32(state.draft_block_index, f);
    blob(state.peer_tip_cbor, f);
    blob(state.own_tip_cbor, f);
    blob(state.own_snap_cbor, f);
    blob(state.peer_snap_cbor, f);

    if (!f) throw std::runtime_error("write error for merge state: " + path.string());
}

static MergeState load_merge_state(const fs::path& data_dir, NodeIndex leaf) {
    const auto path = merge_state_path(data_dir, leaf);
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error(
        "merge state not found — run 'bc merge create --peer-tip HEX' first");

    MergeState state{};
    f.read(reinterpret_cast<char*>(state.partner_pubkey.bytes.data()), 32);

    auto read_le32 = [](std::ifstream& in) -> uint32_t {
        uint8_t b[4];
        in.read(reinterpret_cast<char*>(b), 4);
        return b[0] | (uint32_t(b[1])<<8) | (uint32_t(b[2])<<16) | (uint32_t(b[3])<<24);
    };
    state.draft_block_index = read_le32(f);

    auto read_blob = [&read_le32](std::ifstream& in, std::vector<uint8_t>& out) {
        const uint32_t len = read_le32(in);
        if (!in) return;
        out.resize(len);
        if (len > 0) in.read(reinterpret_cast<char*>(out.data()), len);
    };
    read_blob(f, state.peer_tip_cbor);
    if (!f) throw std::runtime_error("merge state file truncated");

    // Trailing cache blobs are optional (older state files).
    if (f.peek() != EOF) {
        read_blob(f, state.own_tip_cbor);
        read_blob(f, state.own_snap_cbor);
        read_blob(f, state.peer_snap_cbor);
        if (!f) throw std::runtime_error("merge state file truncated");
    }
    return state;
}

// ── Commands ──────────────────────────────────────────────────────────────────

static int cmd_identity_create(const fs::path& data_dir) {
    if (fs::exists(key_path(data_dir, 0))) {
        std::cerr << "Identity already exists in " << data_dir
                  << " — run: bc identity show\n";
        return 1;
    }

    std::cerr << "Generating keys for 32 nodes (root → default branch)...\n";

    const auto path_idxs = path_indices(DEFAULT_LEAF);  // 32 indices: root to leaf
    std::unordered_map<NodeIndex, KeyPair> key_map;
    key_map.reserve(path_idxs.size());
    for (NodeIndex idx : path_idxs) {
        auto kp = Crypto::generate_keypair();
        save_keypair(data_dir, idx, kp);
        key_map[idx] = kp;
    }

    fs::create_directories(data_dir / "db");
    LmdbStorage storage(data_dir / "db");
    Validator   validator(storage);
    Blockchain  bc(storage, validator);

    const KeyPair& root_kp = key_map.at(0);
    bc.create_identity(root_kp);
    bc.ensure_path(root_kp.pub, DEFAULT_LEAF, [&](NodeIndex n) -> KeyPair {
        auto it = key_map.find(n);
        if (it == key_map.end())
            throw std::runtime_error("key not found for node " + std::to_string(n));
        return it->second;
    });

    std::cout << "Identity created.\n";
    std::cout << "User ID: " << to_hex(root_kp.pub.bytes) << "\n";
    std::cout << "Data:    " << fs::absolute(data_dir).string() << "\n";
    return 0;
}

static int cmd_identity_show(const fs::path& data_dir) {
    std::cout << "User ID: " << to_hex(load_user_id(data_dir).bytes) << "\n";
    std::cout << "Data:    " << fs::absolute(data_dir).string() << "\n";
    return 0;
}

static int cmd_branch_init(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [branch, init, <leaf>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc branch init <node_index>\n"
                     "  node_index: decimal or hex (0x...), any tree node 0..0xFFFFFFFE\n";
        return 1;
    }

    NodeIndex leaf = 0;
    try {
        unsigned long long v = std::stoull(pos[2], nullptr, 0);
        if (v > 0xFFFF'FFFEu || !is_valid_node(static_cast<NodeIndex>(v)))
            throw std::runtime_error("not a valid node");
        leaf = static_cast<NodeIndex>(v);
    } catch (...) {
        std::cerr << "Invalid node index: " << pos[2]
                  << " (valid range 0..0xFFFFFFFE)\n";
        return 1;
    }

    if (!fs::exists(key_path(data_dir, 0))) {
        std::cerr << "Identity not found — run: bc identity create\n";
        return 1;
    }

    if (fs::exists(key_path(data_dir, leaf))) {
        std::cout << "Branch " << leaf << " already initialized.\n";
        return 0;
    }

    // Generate keypairs only for nodes that don't have keys yet.
    const auto path_idxs = path_indices(leaf);
    std::unordered_map<NodeIndex, KeyPair> new_keys;
    for (NodeIndex idx : path_idxs) {
        if (!fs::exists(key_path(data_dir, idx))) {
            auto kp = Crypto::generate_keypair();
            save_keypair(data_dir, idx, kp);
            new_keys[idx] = kp;
        }
    }

    LmdbStorage storage(data_dir / "db");
    Validator   validator(storage);
    Blockchain  bc(storage, validator);
    const UserId uid = load_user_id(data_dir);

    bc.ensure_path(uid, leaf, [&](NodeIndex n) -> KeyPair {
        auto it = new_keys.find(n);
        if (it != new_keys.end()) return it->second;
        return load_keypair(data_dir, n);
    });

    std::cout << "Branch " << leaf << " initialized.\n";
    return 0;
}

// Short form of a hex chain id — reports are read by people, not by hash.
static std::string short_hex_str(const std::string& hex) {
    return hex.size() > 8 ? hex.substr(0, 8) + "…" : hex;
}

// ── Catalogs (records.md §8.7) ────────────────────────────────────────────────

// Fetch the aggregator's catalog bundle. Returns nullopt when the aggregator is
// unreachable or serves no catalog — validation then simply does not happen
// (a missing catalog must never block a person from writing about themselves).
static std::optional<std::vector<records::Catalog>> fetch_catalogs(
        const std::string& via) {
    if (via.empty()) return std::nullopt;
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get("/catalog");
    if (!res || res->status != 200) return std::nullopt;
    try {
        return records::parse_catalog_bundle(res->body);
    } catch (const records::CatalogError&) {
        return std::nullopt;
    }
}

// Check every cat: tag against the catalog. A typo in a slug silently breaks
// matching — the person is listed but never found — so it is refused, not warned
// about. --force overrides, for a slug the catalog does not carry yet.
static bool validate_slugs(const std::vector<std::string>& tags,
                           const std::vector<records::Catalog>& catalogs,
                           const std::string& where) {
    const auto known = records::all_slugs(catalogs);
    bool ok = true;
    for (const auto& tag : tags) {
        if (tag.rfind("cat:", 0) != 0) continue;
        const std::string slug = tag.substr(4);
        if (known.count(slug)) continue;
        ok = false;
        std::cerr << where << ": слага \"" << slug << "\" нет в каталоге.\n";
        const auto near = records::search(catalogs, slug.substr(0, slug.find('.') + 1));
        if (!near.empty()) {
            std::cerr << "  похожие: ";
            for (std::size_t i = 0; i < near.size() && i < 6; ++i)
                std::cerr << (i ? ", " : "") << near[i]->slug;
            std::cerr << "\n";
        }
    }
    if (!ok)
        std::cerr << "  найдите слаг: bc catalog --via URL --search ТЕКСТ\n"
                     "  либо запишите как есть: --force (каталог дополнит организатор)\n";
    return ok;
}

static int cmd_concept_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [concept, add, <text>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc concept add <text> [--tag TAG...]\n";
        return 1;
    }
    Concept c;
    c.text = pos[2];
    c.tags = flag_all(argc, argv, "--tag");

    if (!flag_present(argc, argv, "--force")) {
        if (const auto catalogs = fetch_catalogs(flag_val(argc, argv, "--via")))
            if (!validate_slugs(c.tags, *catalogs, "запись")) return 1;
    }
    return cmd_write(data_dir, argc, argv, c);
}

static int cmd_concept_link(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [concept, link, <from>, <to>]
    if (pos.size() < 4) {
        std::cerr << "Usage: bc concept link <from_ref> <to_ref> --kind KIND\n";
        return 1;
    }
    ConceptLink cl;
    cl.from = parse_ref(pos[2]);
    cl.to   = parse_ref(pos[3]);
    cl.kind = flag_val(argc, argv, "--kind", "связь");
    return cmd_write(data_dir, argc, argv,cl);
}

static int cmd_composite_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [composite, add, <title>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc composite add <title> [--part REF...]\n";
        return 1;
    }
    Composite c;
    c.title = pos[2];
    for (const auto& s : flag_all(argc, argv, "--part"))
        c.parts.push_back(parse_ref(s));
    return cmd_write(data_dir, argc, argv,c);
}

static int cmd_copy(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [copy, <ref>]
    if (pos.size() < 2) {
        std::cerr << "Usage: bc copy <chain_hex>/<hash_hex>\n";
        return 1;
    }
    Copy c;
    c.source = parse_ref(pos[1]);
    return cmd_write(data_dir, argc, argv,c);
}

static int cmd_react(const fs::path& data_dir, int argc, char** argv) {
    const auto pos     = get_positionals(argc, argv);  // [react, <hash>]
    const auto val_str = flag_val(argc, argv, "--value");
    if (pos.size() < 2 || val_str.empty()) {
        std::cerr << "Usage: bc react <hash_hex> --value N [-128..+127]\n"
                     "               [--chain CHAIN_HEX]  (default: own chain)\n";
        return 1;
    }
    const int val = std::stoi(val_str);
    if (val < -128 || val > 127) {
        std::cerr << "--value must be in -128..127\n";
        return 1;
    }

    Reaction r;
    r.value = static_cast<int8_t>(val);

    const auto chain_str = flag_val(argc, argv, "--chain");
    if (!chain_str.empty()) {
        if (!from_hex(chain_str, r.target.chain.data(), 32)) {
            std::cerr << "Invalid --chain hex\n";
            return 1;
        }
    } else {
        r.target.chain = load_user_id(data_dir).bytes;
    }

    if (!from_hex(pos[1], r.target.hash.data(), 32)) {
        std::cerr << "Invalid hash hex\n";
        return 1;
    }
    return cmd_write(data_dir, argc, argv,r);
}

// bc work log --action TEXT --hours H [--title TEXT] ...
// The worker is whoever signs the block. There is no specialty or grade to
// point at (ИР-022 шаг 11): «кем работал» is free text for people, and what the
// work was is said by the breakdown of its price at acceptance.
static std::array<uint8_t, 32> uid_from_hex(const std::string& s);

static int cmd_work_log(const fs::path& data_dir, int argc, char** argv) {
    const auto action_s = flag_val(argc, argv, "--action");
    const auto hours_s  = flag_val(argc, argv, "--hours");
    if (action_s.empty() || hours_s.empty()) {
        std::cerr << "Usage: bc work log\n"
                     "    --action \"description\"\n"
                     "    --hours  FLOAT\n"
                     "   [--title  TEXT]            кем работал — вольный текст для людей\n"
                     "   [--for    CHAIN]           заказчик: работа на заказ — результат\n"
                     "                              отпущен ему этой же записью (records.md §9.4);\n"
                     "                              без него — bc release после работы\n"
                     "   [--start  UNIX_TS]         начало; по умолчанию работа только что\n"
                     "                              кончилась. Часы заняты другой работой —\n"
                     "                              отказ, время называет человек; будущим\n"
                     "                              временем работу не кончают (records.md §9.4)\n"
                     "   [--axis-share ОСЬ=ДОЛЯ]    предложенная разбивка часов по осям,\n"
                     "                              сумма 1 — покупатель её подтверждает\n"
                     "   [--input  NAME:QTY:UNIT]   (repeatable)\n"
                     "   [--output ИМЯ:КОЛ:ЕД[:ДОЛЯ[:ЦЕНА[:НОМЕРА[:СРОК]]]]] партия (repeatable,\n"
                     "                              records.md §9.4): ДОЛЯ часов при нескольких\n"
                     "                              выходах; ЦЕНА — ценник, трудочасы за партию\n"
                     "                              или k=K; НОМЕРА — через запятую\n"
                     "   [--tool   HASH_PREFIX:USED] (repeatable — перенос стоимости, ИР-011)\n";
        return 1;
    }
    WorkRecord wr;
    wr.action = action_s;
    wr.hours  = std::stod(hours_s);
    wr.title  = flag_val(argc, argv, "--title");
    // Работа на заказ (records.md §9.4 v3): результат отпущен заказчику этой же
    // записью; без --for — отпуск потом (bc release, records.md §9.8).
    if (const auto for_s = flag_val(argc, argv, "--for"); !for_s.empty())
        wr.customer = uid_from_hex(for_s);
    // Без --start работа считается только что законченной (records.md §9.4 v4).
    const auto start_s = flag_val(argc, argv, "--start");
    wr.start_ts = start_s.empty()
                  ? static_cast<int64_t>(std::time(nullptr)) - std::llround(wr.hours * 3600.0)
                  : static_cast<int64_t>(std::stoll(start_s));
    wr.axes = parse_axis_shares(flag_all(argc, argv, "--axis-share"));
    for (const auto& s : flag_all(argc, argv, "--input"))  wr.inputs.push_back(parse_rq(s));
    std::vector<std::optional<double>> tag_k;   // tags still to compute from k (records.md §9.4 v6)
    for (const auto& s : flag_all(argc, argv, "--output")) {
        auto spec = parse_output(s);
        wr.outputs.push_back(std::move(spec.rq));
        tag_k.push_back(spec.k);
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    auto carry_specs = flag_all(argc, argv, "--tool");
    for (const auto& s : flag_all(argc, argv, "--material")) carry_specs.push_back(s);
    if (!carry_specs.empty()) attach_carry(ctx, wr, carry_specs);
    place_in_thread(ctx, wr, !start_s.empty());

    // Ценник из k (records.md §9.4 v6, ИР-023, вопрос 2): часы × Σ цен
    // предложенных осей дня производства × k — закрепляется числом трудочасов
    // за всю партию; порции потом продаются по нему, «бери или нет».
    if (std::any_of(tag_k.begin(), tag_k.end(), [](const auto& k) { return k.has_value(); })) {
        if (wr.axes.empty())
            throw std::runtime_error("ценник из k считается по предложенным осям — задайте "
                                     "--axis-share (records.md §9.4)");
        const auto via_s  = flag_val(argc, argv, "--via");
        auto       prices = lookup_axis_prices_on(via_s, wr.start_ts);
        if (prices.empty()) prices = lookup_axis_prices(via_s);
        double      rate   = 0.0;
        std::size_t priced = 0;
        for (const auto& x : wr.axes)
            if (const auto it = prices.find(x.axis.hash); it != prices.end()) {
                rate += it->second;
                ++priced;
            }
        if (priced == 0) {
            rate = 1.0;
            std::cerr << "у предложенных осей ещё нет сетевой цены — 1.0 стч/ч\n";
        }
        for (std::size_t i = 0; i < wr.outputs.size(); ++i) {
            if (!tag_k[i]) continue;
            auto& o = wr.outputs[i];
            if (wr.outputs.size() > 1 && !o.share)
                throw std::runtime_error("у нескольких выходов ценник из k делится по долям — "
                                         "объявите их: --output ИМЯ:КОЛ:ЕД:ДОЛЯ:k=K");
            o.price = wr.hours * (o.share ? *o.share : 1.0) * rate * *tag_k[i];
            std::cerr << "ценник «" << o.resource << "»: " << qty_str(*o.price) << " стч за "
                      << qty_str(o.qty) << " " << o.unit << " (k = " << *tag_k[i] << ")\n";
        }
    }

    const Block block = append_record(ctx, Record{wr});
    std::cout << "block #" << block.address.block_index
              << "  hash: " << to_hex(Crypto::hash_block(block).bytes) << "\n";
    if (wr.customer)
        std::cerr << "на заказ: результат отпущен " << short_hex(*wr.customer)
                  << " этой же записью (records.md §9.4)\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// Payment and JSON helpers live further down, with the economy and aggregator
// verbs.
static std::vector<records::OriginQty> pick_portions(Context& ctx,
                                                     const std::string& to_hex_s,
                                                     double units);
static void attach_emission_link(Context& ctx, records::Transfer& t);
static void print_portions(const records::Transfer& t, const std::string& me_hex,
                           const std::string& to_hex_s);
static std::optional<records::json::Value> fetch_json(const std::string& via,
                                                      const std::string& path);
static std::string jstr(const records::json::Object& o, const char* k);
static double jnum(const records::json::Object& o, const char* k);
static const records::json::Value* jarr(const records::json::Object& o,
                                        const char* k);

// The own deal on `work` already on record in the own branches: its ref, or
// empty. One result — one deal (records.md §9.5, ИР-023): a second deal of the
// same buyer buys nothing, it only overpays. A portion of a lot is a result of
// its own (records.md §9.8 v2): `portion` narrows the search to that release.
static std::string own_deal_on(Context& ctx, const Ref& work,
                               const std::optional<Ref>& portion = std::nullopt) {
    for (const NodeIndex node : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, node); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }
            const auto* t = std::get_if<records::Transfer>(&rec);
            if (t && t->accept && t->accept->work == work &&
                (!portion || t->accept->release == portion))
                return to_hex(ctx.user_id.bytes) + "/" + to_hex(Crypto::hash_block(b).bytes);
        }
    }
    return {};
}

// The aggregator's view of one result (GET /work/<chain>/<hash>, records.md §9.5):
// the work's customer, its releases and every deal naming it with its standing.
// nullopt without --via or when the aggregator is unreachable.
static std::optional<records::json::Value> fetch_work_status(const std::string& via,
                                                             const Ref& work) {
    if (via.empty()) return std::nullopt;
    return fetch_json(via, "/work/" + to_hex(work.chain.data(), 32) + "/" +
                           to_hex(work.hash.data(), 32));
}

// A purchase as a lot to sell on (records.md §9.8 v4): the buyer's deal, the
// product's birth record and the release the deal completed give its lots. Any
// chain's purchase — the records come from the own chain, the local store or the
// aggregator. nullopt when one of them cannot be had.
struct Holding {
    records::Transfer                deal;
    WorkRecord                       work;
    std::optional<records::Release>  bought;
    std::vector<records::HoldingLot> lots;
};

static std::optional<Holding> load_holding(Context& ctx, const std::string& via,
                                           const Ref& purchase) {
    const auto  drec = load_record(ctx, via, purchase);
    const auto* deal = drec ? std::get_if<records::Transfer>(&*drec) : nullptr;
    if (!deal || !deal->accept || deal->from != purchase.chain) return std::nullopt;
    const auto  wrec = load_record(ctx, via, deal->accept->work);
    const auto* work = wrec ? std::get_if<WorkRecord>(&*wrec) : nullptr;
    if (!work) return std::nullopt;
    Holding h{*deal, *work, std::nullopt, {}};
    if (deal->accept->release) {
        const auto  rrec = load_record(ctx, via, *deal->accept->release);
        const auto* rl   = rrec ? std::get_if<records::Release>(&*rrec) : nullptr;
        if (!rl) return std::nullopt;
        h.bought = *rl;
    }
    h.lots = records::holding_lots(h.deal, h.work, h.bought ? &*h.bought : nullptr);
    return h;
}

// How a whole result is appraised (records.md §9.5): the buyer's breakdown by
// axis — named, as shares, like a sample's, or the worker's proposal — and the
// price: named hours, a manual rate, or hours × Σ network prices of the axes ×
// k. This is where the parties bargain: over an order or a lot bought whole
// (ИР-023, вопрос 11); a portion from stock is not appraised, it has its terms.
// The breakdown is written out in full and checked against the price: the price
// and its explanation must agree exactly (ИР-022).
static void appraise_whole(Context& ctx, const std::string& via, int argc, char** argv,
                           const WorkRecord* work, Acceptance& a,
                           const std::vector<std::string>& axis_args,
                           const std::vector<std::string>& share_args,
                           const std::string& like_s, const std::string& raw_s,
                           const std::string& lu_s) {
    for (const auto& p : axis_args) {
        const auto eq = p.rfind('=');
        if (eq == std::string::npos || eq == 0)
            throw std::runtime_error("--axis ждёт ПУТЬ_ОСИ=ЧАСЫ (путь: цепь/узел/блок/хеш): " + p);
        AcceptanceAxis x{};
        x.axis  = parse_block_path(p.substr(0, eq));
        x.units = std::stod(p.substr(eq + 1));
        a.axes.push_back(std::move(x));
    }
    std::vector<std::pair<BlockPath, double>> shares;
    for (const auto& p : share_args) {
        const auto eq = p.rfind('=');
        if (eq == std::string::npos || eq == 0)
            throw std::runtime_error("--axis-share ждёт ПУТЬ_ОСИ=ДОЛЯ (путь: цепь/узел/блок/хеш): " + p);
        shares.emplace_back(parse_block_path(p.substr(0, eq)), std::stod(p.substr(eq + 1)));
    }

    // «Такая же работа, как та» (ИР-022 шаг 11): unless this deal names its own
    // breakdown, the sample's is taken as shares. The reference stays in the
    // record as provenance — the work cloud is made of it.
    if (!like_s.empty()) {
        a.like = parse_ref(like_s);
        if (a.axes.empty() && shares.empty()) {
            const auto  rec    = load_record(ctx, via, *a.like);
            const auto* deal   = rec ? std::get_if<records::Transfer>(&*rec) : nullptr;
            const Acceptance* sample = deal && deal->accept ? &*deal->accept : nullptr;
            if (!sample)
                throw std::runtime_error("--like: образец не найден или это не сделка с "
                                         "приёмкой (bc fetch <ref> --via URL)");
            if (sample->labor_units <= 0.0)
                throw std::runtime_error("--like: у образца нулевая цена — долей из неё не вывести");
            for (const auto& x : sample->axes)
                shares.emplace_back(x.axis, x.units / sample->labor_units);
            std::cerr << "разбивка как у образца: " << shares.size() << " ос.\n";
        }
    }

    // Ничего не названо — разбивка, которую предложил работник (records.md §9.4
    // v4): покупатель подтверждает её своей подписью.
    if (a.axes.empty() && shares.empty() && work && !work->axes.empty()) {
        for (const auto& x : work->axes) shares.emplace_back(x.axis, x.share);
        std::cerr << "разбивка, предложенная работником: " << shares.size() << " ос.\n";
    }
    if (a.axes.empty() && shares.empty())
        throw std::runtime_error("разбивка цены обязательна (ИР-022): --axis, --axis-share или "
                                 "--like — у работы нет предложенной (records.md §9.4)");

    if (raw_s.empty()) {
        // Hours from the worker's record (records.md §9.5).
        if (!work)
            throw std::runtime_error(
                "запись работы не найдена — 'bc fetch <ref> --via URL' или --via, "
                "или задайте --hours-raw");
        a.hours_raw = work->hours;
    } else {
        a.hours_raw = std::stod(raw_s);
    }

    // The price of live labour (economy.md §2а).
    if (!lu_s.empty()) {
        a.labor_units = std::stod(lu_s);
    } else if (!a.axes.empty()) {
        for (const auto& x : a.axes) a.labor_units += x.units;   // hours named ARE the price
    } else {
        // hours × Σ network price of the axes named × k — or a manual rate.
        const auto coef_s = flag_val(argc, argv, "--coef");
        double rate = 0.0;
        if (!coef_s.empty()) {
            rate = std::stod(coef_s);
        } else {
            const auto prices = lookup_axis_prices(via);
            std::size_t priced = 0;
            for (const auto& [path, share] : shares) {
                (void)share;
                if (const auto it = prices.find(path.hash); it != prices.end()) {
                    rate += it->second;
                    ++priced;
                }
            }
            if (priced > 0) {
                std::cerr << "сетевая цена названных осей: " << rate << " стч/ч ("
                          << priced << " из " << shares.size() << " оценены сетью)\n";
            } else {
                rate = 1.0;
                std::cerr << "у названных осей ещё нет сетевой цены — 1.0 стч/ч "
                             "(задайте --coef или --labor-units)\n";
            }
        }
        const double k = std::stod(flag_val(argc, argv, "--k", "1.0"));
        a.labor_units = a.hours_raw * rate * k;
    }

    // v2 (ИР-011): carried cost of the accepted work. carried_units must equal
    // Σ carried of the WorkRecord's carry, or the acceptance is unrecognisable
    // (records.md §9.5) — so it is always derived, never passed by hand.
    if (const auto wblock = find_external_by_hash(ctx, a.work.hash)) {
        if (wblock->type == BlockType::DATA) {
            try {
                const auto wrec = Codec::decode(wblock->payload.data(),
                                                wblock->payload.size());
                const auto* wr = std::get_if<WorkRecord>(&wrec);
                if (wr && !wr->carry.empty()) {
                    double carried = 0;
                    for (const auto& ce : wr->carry) carried += ce.carried;
                    a.carried_units = carried;
                    std::cerr << "перенос средств производства: " << carried
                              << "ч, позиции:\n";
                    for (const auto& ce : wr->carry) {
                        const auto ablock = find_external_by_hash(ctx, ce.src.hash);
                        std::optional<AssetInfo> info;
                        if (ablock && ablock->type == BlockType::DATA) {
                            try {
                                info = asset_info(Codec::decode(
                                    ablock->payload.data(), ablock->payload.size()));
                            } catch (const CodecError&) {}
                        }
                        if (!info) {
                            std::cerr << "  " << short_hex(ce.src.hash) << ": "
                                      << ce.carried << "ч — запись средства не "
                                      "загружена, формула не проверена (bc fetch "
                                      << to_hex(ce.src.chain) << "/"
                                      << to_hex(ce.src.hash) << ")\n";
                            continue;
                        }
                        const double expected = records::carry_step(
                            info->cost, info->capacity, ce.used,
                            ce.after - ce.carried);
                        if (std::abs(expected - ce.carried) > 1e-6)
                            std::cerr << "  ⚠ " << info->name << ": " << ce.carried
                                      << "ч НЕ по формуле переноса (ожидалось "
                                      << expected << "ч, records.md §9.4) — вправе отклонить\n";
                        else
                            std::cerr << "  " << info->name << ": " << ce.carried
                                      << "ч за " << ce.used
                                      << (info->is_material ? " " + info->unit
                                                            : "ч работы")
                                      << (info->basis == "est"
                                          ? "  [оценка владельца]" : "") << "\n";
                    }
                }
            } catch (const CodecError&) {}
        }
    }

    if (!shares.empty()) {
        double sum = 0.0;
        for (const auto& [ref, f] : shares) { (void)ref; sum += f; }
        if (std::abs(sum - 1.0) > 1e-9)
            throw std::runtime_error("--axis-share в сумме даёт " + std::to_string(sum)
                                     + ", а должно 1.0");
        double left = a.labor_units;
        for (std::size_t i = 0; i < shares.size(); ++i) {
            AcceptanceAxis x{};
            x.axis  = shares[i].first;
            // Последней достаётся остаток: сумма обязана совпасть с ценой ТОЧНО,
            // а не с точностью до округления долей.
            x.units = i + 1 == shares.size() ? left : a.labor_units * shares[i].second;
            left   -= x.units;
            a.axes.push_back(std::move(x));
        }
    }
    std::sort(a.axes.begin(), a.axes.end(),
              [](const AcceptanceAxis& x, const AcceptanceAxis& y) {
                  return x.axis.hash < y.axis.hash;
              });
    {   // Разбивка ЕСТЬ цена: расхождение здесь — не округление, а другая сделка.
        double named = 0.0;
        for (const auto& x : a.axes) named += x.units;
        if (std::abs(named - a.labor_units) > 1e-6)
            throw std::runtime_error(
                "разбивка по осям даёт " + std::to_string(named) + " стч, а оценка — "
                + std::to_string(a.labor_units) + " стч. Цена и её объяснение обязаны "
                  "совпадать в точности (ИР-022)");
    }

}

static int cmd_accept(const fs::path& data_dir, int argc, char** argv) {
    const auto work_s    = flag_val(argc, argv, "--work");
    const auto quality_s = flag_val(argc, argv, "--quality");
    const auto raw_s     = flag_val(argc, argv, "--hours-raw");
    const auto lu_s      = flag_val(argc, argv, "--labor-units");
    const auto like_s    = flag_val(argc, argv, "--like");
    const auto via       = flag_val(argc, argv, "--via");
    std::vector<std::string> axis_args, share_args;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--axis")       axis_args.emplace_back(argv[i + 1]);
        if (std::string(argv[i]) == "--axis-share") share_args.emplace_back(argv[i + 1]);
    }
    if (work_s.empty() || quality_s.empty()) {
        std::cerr << "Usage: bc accept — принять работу и оплатить её одной записью\n"
                     "    --work         WORK_CHAIN/HASH\n"
                     "    --quality      TEXT\n"
                     "  разбивка цены обязательна (ИР-022) — одним из способов:\n"
                     "    --axis ПУТЬ_ОСИ=ЧАСЫ        сколько трудочасов цены ушло на эту\n"
                     "                                 ось; повторяйте для каждой. Сумма\n"
                     "                                 и есть цена\n"
                     "    --axis-share ОСЬ=ДОЛЯ        доли цены (в сумме 1.0); остаток\n"
                     "                                 отдаётся последней оси — чтобы\n"
                     "                                 округление не разошлось с ценой\n"
                     "    --like ПРИЁМКА               «такая же работа, как та»: доли\n"
                     "                                 берутся у образца, ссылка на него\n"
                     "                                 остаётся в приёмке\n"
                     "    (ничего не задано)           разбивка, предложенная работником в\n"
                     "                                 записи труда (records.md §9.4)\n"
                     "  порция партии — по ценнику, «бери или нет» (records.md §9.8): её\n"
                     "  разбивка, часы и цена берутся из партии, свои не задаются\n"
                     "    [--hours-raw   FLOAT]   default: hours of the fetched WorkRecord\n"
                     "    [--k           FLOAT]   price = hours × Σ network prices of the\n"
                     "                            axes named × k (default k 1.0)\n"
                     "    [--coef        FLOAT]   manual rate, стч/ч, instead of axis prices\n"
                     "    [--labor-units FLOAT]   the price outright\n"
                     "    [--pledge      REF]     the own pledge this deal closes (v4)\n"
                     "    [--release     REF]     the seller's release to you (records.md §9.8);\n"
                     "                            default: found at the aggregator. Not needed\n"
                     "                            when the work names you as its customer\n"
                     "    [--buyback-of  REF]     buy back a purchase on its pre-signed return\n"
                     "                            (default: found at the aggregator)\n";
        return 1;
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    Acceptance a;
    a.work        = parse_ref(work_s);
    a.quality     = quality_s;
    a.labor_units = 0.0;

    const auto status = fetch_work_status(via, a.work);

    // Кому достаётся результат, решает продавец (records.md §9.5): заказчик в
    // записи труда (records.md §9.4) или отпуск на имя покупателя
    // (records.md §9.8). Сделку не по адресу не пишем — она ничтожна, часы по
    // ней никуда не перейдут.
    const auto        now      = static_cast<int64_t>(std::time(nullptr));
    const auto        work_rec = load_record(ctx, via, a.work);
    const WorkRecord* work     = work_rec ? std::get_if<WorkRecord>(&*work_rec) : nullptr;
    const std::optional<std::array<uint8_t, 32>> customer =
        work ? work->customer : std::optional<std::array<uint8_t, 32>>{};
    std::optional<records::Release> release;
    if (const auto rs = flag_val(argc, argv, "--release"); !rs.empty()) {
        const Ref  rref = parse_ref(rs);
        const auto rrec = load_record(ctx, via, rref);
        const auto* rl  = rrec ? std::get_if<records::Release>(&*rrec) : nullptr;
        if (!rl)
            throw std::runtime_error("--release: отпуск не найден (bc fetch <ref> --via URL)");
        release   = *rl;
        a.release = rref;
    } else if (!(customer && *customer == ctx.user_id.bytes) && status &&
               status->is_object()) {
        // Не заказчик — ищем живой отпуск на своё имя; из нескольких — самый долгий.
        if (const auto* rels = jarr(status->object, "releases"))
            for (const auto& r : rels->array) {
                if (!r.is_object() || jstr(r.object, "to") != to_hex(ctx.user_id.bytes))
                    continue;
                if (jnum(r.object, "expires") <= static_cast<double>(now)) continue;
                if (const auto* tk = records::json::find(r.object, "taken"); tk && tk->boolean)
                    continue;   // already bought
                const Ref  rref = parse_ref(jstr(r.object, "ref"));
                const auto rrec = load_record(ctx, via, rref);
                const auto* rl  = rrec ? std::get_if<records::Release>(&*rrec) : nullptr;
                if (rl && (!release || rl->expires > release->expires)) {
                    release   = *rl;
                    a.release = rref;
                }
            }
    }
    // Выкуп по заранее подписанному возврату (records.md §9.8 v5): отпуском служит
    // сама покупка держателя — адресат выкупает сам, без него.
    std::optional<records::Transfer> held;   // the holder's purchase bought back
    if (!release) {
        std::optional<Ref> bref;
        if (const auto bs = flag_val(argc, argv, "--buyback-of"); !bs.empty()) {
            bref = parse_ref(bs);
        } else if (status && status->is_object()) {
            if (const auto* bbs = jarr(status->object, "buybacks"))
                for (const auto& x : bbs->array) {
                    if (!x.is_object() || jstr(x.object, "to") != to_hex(ctx.user_id.bytes))
                        continue;
                    if (jnum(x.object, "until") <= static_cast<double>(now)) continue;
                    if (const auto* tk = records::json::find(x.object, "taken"); tk && tk->boolean)
                        continue;
                    bref = parse_ref(jstr(x.object, "ref"));
                }
        }
        if (bref) {
            const auto  prec = load_record(ctx, via, *bref);
            const auto* p    = prec ? std::get_if<records::Transfer>(&*prec) : nullptr;
            if (!p || !p->accept || !p->accept->buyback)
                throw std::runtime_error("--buyback-of: покупки с подписанным возвратом не найти — "
                                         "нужен --via");
            held      = *p;
            a.release = *bref;
        }
    }
    // Кому платим: работнику — или держателю, если это перепродажа его покупки
    // (records.md §9.8 v4) или выкуп по её возврату (v5). Самоприёмка ничего не
    // заверяет (records.md §14.4) — кодек её и не запишет.
    const bool resale = held || (release && release->holding);
    const std::array<uint8_t, 32> seller = held ? held->from
                                         : resale ? release->holding->chain : a.work.chain;
    if (seller == ctx.user_id.bytes)
        throw std::runtime_error(resale ? "это ваша же перепродажа — у себя не покупают"
                                        : "свою работу не принимают — принимает и платит "
                                          "заказчик (records.md §9.5)");
    // Цену возврата не называет никто (records.md §9.8 v6): уплаченное держателем, а
    // для аренды — за вычетом износа по норме срока службы.
    double back_price = 0.0;
    if (held) {
        const records::Buyback& b = *held->accept->buyback;
        if (b.to != ctx.user_id.bytes || now >= b.until)
            throw std::runtime_error("возврат подписан не вам или его срок прошёл "
                                     "(records.md §9.8)");
        if (!(held->accept->work == a.work))
            throw std::runtime_error("возврат этой покупки — о другой вещи (--work)");
        const auto h = load_holding(ctx, via, *a.release);
        if (!h) throw std::runtime_error("покупку держателя не прочесть — нужен --via");
        double paid = 0.0;
        for (const auto& l : h->lots) paid += l.cost;
        back_price = records::buyback_price(h->lots, h->work, held->timestamp, now, b.wear);
        std::cerr << "выкуп по подписанному возврату: " << back_price << " стч";
        if (b.wear)
            std::cerr << " — из уплаченных " << paid << " за вычетом износа по норме "
                      << paid - back_price << " (аренда: только износ, records.md §9.8)";
        std::cerr << ", до " << b.until << " — держатель в сделке не участвует\n";
    } else {
        records::Transfer probe{};
        probe.from      = ctx.user_id.bytes;
        probe.timestamp = now;
        probe.accept    = a;
        if (!records::addressed_to(probe, customer, release ? &*release : nullptr)) {
            if (release)
                throw std::runtime_error(
                    "отпуск не на вас, не на эту работу или истёк (срок " +
                    std::to_string(release->expires) + ") — попросите продавца "
                    "отпустить заново (bc release, records.md §9.8)");
            throw std::runtime_error(
                "работа не отпущена вам: результат достаётся тому, кого назвал продавец — "
                "заказчиком в записи труда (bc work log --for) или отпуском на ваше имя "
                "(bc release). Сделка не по адресу ничтожна (records.md §9.5)");
        }
        std::cerr << (release ? "по отпуску продавца: " + short_hex(a.release->hash) +
                                    ", до " + std::to_string(release->expires)
                              : std::string("работа на ваш заказ"))
                  << "\n";
    }
    // Нить труда продавца (records.md §9.4 v4): платим только за работу в нити и на
    // своих часах — иначе эти часы могли продать дважды, и сделка в счёт не пойдёт.
    if (!resale && status && status->is_object()) {
        const auto state = jstr(status->object, "thread_state");
        if (state == "unthreaded")
            throw std::runtime_error(
                "работа записана вне нити труда — её часов не сверить с другими работами "
                "продавца, в счёт такая сделка не пойдёт (records.md §9.4)");
        if (state == "broken")
            throw std::runtime_error("звено нити труда битое — работа вне нити (records.md §9.4)");
        if (state == "ahead")
            throw std::runtime_error(
                "работа кончается позже, чем записана, — это часы из будущего, которых ещё "
                "никто не отработал; в счёт такая сделка не пойдёт (records.md §9.4)");
        if (state == "crossing") {
            std::string with;
            if (const auto* cs = jarr(status->object, "crosses"))
                for (const auto& c : cs->array)
                    if (c.is_string()) with += " " + c.string.substr(c.string.find('/') + 1, 16);
            throw std::runtime_error(
                "часы этой работы заняты и другой работой продавца:" + with +
                " — одни и те же часы проданы дважды, в счёт не пойдёт ни одна "
                "(records.md §9.4)");
        }
    }
    // Порция партии (records.md §9.8 v3) — «бери или нет»: её цена — доля ценника
    // партии по разбивке, предложенной работником, её часы и перенос — доля
    // работы. Торгуются при заказе и при покупке партии целиком (ИР-023, вопрос 11).
    const bool                           portion = release && release->lot && !resale;
    std::optional<records::PortionTerms> terms;
    if (portion) {
        if (!work)
            throw std::runtime_error("порция партии: запись работы не найдена — нужен --via");
        terms = records::portion_terms(*work, *release->lot);
        if (!terms)
            throw std::runtime_error("порцию не оценить: у партии нет ценника или у работы — "
                                     "предложенной разбивки, или у нескольких выходов не "
                                     "объявлены доли (records.md §9.8)");
        const auto& o = work->outputs[release->lot->out];
        std::cerr << "порция партии «" << o.resource << "»: " << qty_str(release->lot->qty)
                  << " " << o.unit << " из " << qty_str(o.qty);
        for (std::size_t i = 0; i < release->lot->units.size(); ++i)
            std::cerr << (i ? ", " : " (номера ") << release->lot->units[i]
                      << (i + 1 == release->lot->units.size() ? ")" : "");
        std::cerr << " — по ценнику: " << terms->labor_units << " стч\n";
    }
    // Перепродажа (records.md §9.8 v4): цена — доля того, что держатель заплатил за
    // купленное, без наценки и без труда; оси определены при первой продаже.
    double resale_units = 0.0;
    if (held) {
        resale_units = back_price;
    } else if (resale) {
        const auto h = load_holding(ctx, via, *release->holding);
        const records::HoldingLot* hl = nullptr;
        if (h)
            for (const auto& l : h->lots)
                if (l.out == release->lot->out) hl = &l;
        if (!hl)
            throw std::runtime_error("перепродажа: покупки держателя не найти или в ней нет "
                                     "такой партии — нужен --via");
        resale_units = records::resale_price(*hl, release->lot->qty);
        std::cerr << "перепродажа: " << qty_str(release->lot->qty) << " из купленных "
                  << "держателем " << qty_str(hl->qty) << " — по цене покупки: " << resale_units
                  << " стч, без наценки (records.md §9.8)\n";
    }

    // Один результат — одна сделка (records.md §9.5, ИР-023); порция — свой
    // результат. Своя повторная ничего не купит, только переплатит; сделка рядом с
    // чужой по адресу сделала бы результат «проданным дважды». Обхода нет.
    const std::string release_ref =
        a.release ? to_hex(a.release->chain) + "/" + to_hex(a.release->hash) : std::string{};
    const bool        piece     = portion || resale;   // a result of its own: its release
    const std::string my_source =
        held     ? to_hex(a.release->chain) + "/" + to_hex(a.release->hash)
        : resale ? to_hex(release->holding->chain) + "/" + to_hex(release->holding->hash)
                 : to_hex(a.work.chain) + "/" + to_hex(a.work.hash);
    if (const auto mine = own_deal_on(ctx, a.work, piece ? a.release : std::optional<Ref>{});
        !mine.empty())
        throw std::runtime_error(std::string("вы уже приняли и оплатили ") +
                                 (piece ? "эту порцию: " : "эту работу: ") + mine +
                                 " (records.md §9.5)");
    if (status && status->is_object()) {
        if (const auto* ds = jarr(status->object, "deals"))
            for (const auto& d : ds->array) {
                if (!d.is_object() || jstr(d.object, "state") == "unaddressed") continue;
                if (jstr(d.object, "source") != my_source) continue;   // другой источник
                const auto* p = records::json::find(d.object, "portion");
                const bool theirs_portion = p && p->boolean;
                // Целиком — ни рядом с какой; порция — не рядом с продажей целиком и
                // не та же порция второй раз.
                if (!piece || !theirs_portion || jstr(d.object, "release") == release_ref)
                    throw std::runtime_error(
                        "работа уже продана: сделка " + jstr(d.object, "ref") +
                        ". Один результат — одна сделка (records.md §9.5): вторая по "
                        "адресу сделала бы его «проданным дважды»");
            }
        // Звено партии, которое не сходится, — порция сверх партии или продажа из
        // одного остатка дважды: платить за неё — отдать часы за чужой обман.
        if (piece)
            if (const auto* rs = jarr(status->object, "releases"))
                for (const auto& r : rs->array)
                    if (r.is_object() && jstr(r.object, "ref") == release_ref)
                        if (const auto* v = records::json::find(r.object, "valid"); v && !v->boolean)
                            throw std::runtime_error(
                                "звено партии не сходится — порция сверх партии или продана из "
                                "одного остатка дважды; в счёт не пойдёт (records.md §9.8)");
    }

    if (resale) {
        a.hours_raw     = 0.0;
        a.labor_units   = 0.0;
        a.carried_units = resale_units;
        a.axes.clear();
        if (!axis_args.empty() || !share_args.empty() || !like_s.empty() || !raw_s.empty() ||
            !lu_s.empty() || !flag_val(argc, argv, "--coef").empty() ||
            !flag_val(argc, argv, "--k").empty())
            std::cerr << "перепродажа — по цене покупки: свои разбивка и цена не принимаются\n";
    } else if (portion) {
        a.hours_raw   = terms->hours_raw;
        a.labor_units = terms->labor_units;
        if (terms->carried_units > 0.0) a.carried_units = terms->carried_units;
        a.axes = terms->axes;
        if (!axis_args.empty() || !share_args.empty() || !like_s.empty() || !raw_s.empty() ||
            !lu_s.empty() || !flag_val(argc, argv, "--coef").empty() ||
            !flag_val(argc, argv, "--k").empty())
            std::cerr << "порция — по ценнику: свои разбивка, часы и цена не принимаются\n";
        if (a.carried_units)
            std::cerr << "перенос средств производства: " << *a.carried_units
                      << "ч (доля порции)\n";
    } else {
        appraise_whole(ctx, via, argc, argv, work, a, axis_args, share_args, like_s, raw_s, lu_s);
    }
    // Выкуп с адресом (records.md §9.8 v5, v6): покупая, подписываем возврат —
    // адресату, до срока, на тех же условиях износа. Цены в нём нет: уплаченное, а
    // для аренды — остаток после износа по норме. Без возврата отпуск не завершить.
    if (release && release->addressed) {
        records::Buyback b{};
        b.to    = release->addressed->to;
        b.until = release->addressed->until;
        b.wear  = release->addressed->wear;
        a.buyback = b;
        std::cerr << (b.wear ? "аренда: подписываю возврат " : "выкуп с адресом: подписываю "
                                                               "возврат ")
                  << short_hex(b.to) << " до " << b.until
                  << (b.wear ? " по остатку после износа по норме — плачу только износ"
                             : " по цене покупки")
                  << "; до срока продать это можно только адресату\n";
    }

    // v3 (economy.md §2б): record the normalizer this appraisal was made against,
    // best-effort — so the оценка reads unambiguously later. Never blocks signing.
    if (const auto nm = fetch_norm(via)) {
        a.norm = *nm;
        std::cerr << "нормировщик W=" << nm->W << " (из агрегатора, для истории)\n";
    }

    // Приёмка — внутри своей оплаты (records.md §9.5, ИР-023): оценка и перевод
    // подписываются одной записью, поэтому оплата равна оценке по построению, а
    // платит тот, кто принимает.
    const double payable = a.labor_units + (a.carried_units ? *a.carried_units : 0.0);
    const std::string me_hex = to_hex(ctx.user_id.bytes);
    const std::string to_s   = to_hex(seller.data(), 32);
    records::Transfer t{};
    t.from      = ctx.user_id.bytes;
    t.to        = seller;
    t.to_node   = DEFAULT_LEAF;
    t.origins   = pick_portions(ctx, to_s, payable);
    t.accept    = a;
    t.timestamp = now;   // the same moment the addressing was checked against
    // Transfer v4 (records.md §11.1): which own promise this deal closes.
    const auto pledge_s = flag_val(argc, argv, "--pledge");
    if (!pledge_s.empty()) t.settles = parse_ref(pledge_s);
    // Pay into the purse of the branch that did the work (economy.md §5а) — for a
    // resale, the branch that wrote the release.
    if (const auto seller_block =
            find_external_by_hash(ctx, resale ? a.release->hash : a.work.hash))
        t.to_node = seller_block->address.node_index;
    attach_emission_link(ctx, t);   // self-issues are threaded (economy.md §4.3)

    const Block block = append_record(ctx, Record{t});
    print_portions(t, me_hex, to_s);
    std::cout << "acceptance ref: " << me_hex << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    if (resale) {
        std::cerr << "куплено у держателя одной записью: " << payable
                  << "ч — перепродажа, без труда (records.md §9.8)\n";
    } else {
        std::cerr << "принято и оплачено одной записью: " << a.labor_units
                  << " labor-h (raw " << a.hours_raw << "h)";
        if (a.carried_units) std::cerr << " + перенос " << *a.carried_units << "ч";
        std::cerr << " = paid " << payable << "h\n";
    }
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// A work named on the command line as the own chain's: its hash alone, or
// CHAIN/HASH that must be the own chain.
static Ref own_work_arg(Context& ctx, const std::string& arg) {
    Ref work{};
    if (arg.find('/') == std::string::npos) {
        work.chain = ctx.user_id.bytes;
        if (!from_hex(arg, work.hash.data(), 32))
            throw std::runtime_error("РАБОТА: ожидался хеш записи труда (64 hex) или ЦЕПЬ/ХЕШ");
    } else {
        work = parse_ref(arg);
    }
    if (work.chain != ctx.user_id.bytes)
        throw std::runtime_error("это чужая запись — отпускает и списывает тот, у кого "
                                 "результат: кто его сделал или купил (records.md §9.8)");
    return work;
}

// The own links of one lot (records.md §9.8 v2): releases of portions and
// write-offs of output `out` of `work` — of the work's own output, or, with
// `holding`, of a purchase sold on (v4) — every local branch. `whole` — the
// work was released whole; `portions` — some output of it goes by portions.
struct OwnLot {
    std::vector<records::ObservedLotLink> links;
    bool whole    = false;
    bool portions = false;
};

static OwnLot own_lot(Context& ctx, const Ref& work, uint32_t out,
                     const std::optional<Ref>& holding = std::nullopt) {
    OwnLot lot;
    for (const NodeIndex node : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, node); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }
            const auto hash = Crypto::hash_block(b).bytes;
            if (const auto* rl = std::get_if<records::Release>(&rec);
                rl && rl->work == work && rl->holding == holding) {
                if (!rl->lot) { lot.whole = true; continue; }
                lot.portions = true;
                if (rl->lot->out == out)
                    lot.links.push_back({hash, *rl->lot, rl->timestamp, rl->expires, false});
            } else if (const auto* wo = std::get_if<records::WriteOff>(&rec);
                       wo && wo->work == work && wo->holding == holding) {
                lot.portions = true;
                if (wo->lot.out == out)
                    lot.links.push_back({hash, wo->lot, wo->timestamp, std::nullopt, false});
            } else if (const auto* m = std::get_if<records::Material>(&rec);
                       m && m->lot && holding && m->src && *m->src == *holding) {
                // Взятое в своё производство — звено той же подниточки покупки
                // (records.md §10.1 v3): что израсходовано, того уже не продать.
                lot.portions = true;
                if (m->lot->out == out)
                    lot.links.push_back({hash, *m->lot, b.timestamp_claimed, std::nullopt,
                                         false, true});
            } else if (const auto* tl = std::get_if<records::Tool>(&rec);
                       tl && tl->lot && holding && tl->src && *tl->src == *holding) {
                lot.portions = true;
                if (tl->lot->out == out)
                    lot.links.push_back({hash, *tl->lot, b.timestamp_claimed, std::nullopt,
                                         false, true});
            }
        }
    }
    return lot;
}

// A lot as its seller sees it: which output of the product, how much of it and
// which units (records.md §9.8).
struct LotView {
    uint32_t                 out = 0;
    double                   qty = 0;
    std::vector<std::string> units;
    std::string              resource, unit;
};

// The next link of a lot's sub-thread (records.md §9.8 v3): after the newest own
// link of the lot — of the work's own output, or of a purchase sold on
// (`holding`, v4) — taking `qty`, or, for a lot counted piece by piece, the
// numbered units asked for (the first free ones when none are named). With
// --via the releases of the lot that expired with nobody taking them go back
// into stock by this very link (`returns`); whether a release was taken only the
// aggregator knows, so without it nothing is returned. A portion the lot no
// longer holds, or a unit already out, is refused.
static records::LotLink take_from_lot(Context& ctx, const std::string& via, const Ref& product,
                                      const std::optional<Ref>& holding, const LotView& o,
                                      std::optional<double> qty, std::vector<std::string> want,
                                      int64_t now) {
    const bool numbered = !o.units.empty();
    if (!numbered && !want.empty())
        throw std::runtime_error("у партии «" + o.resource + "» нет номеров единиц — её "
                                 "отпускают количеством (--qty)");
    const OwnLot lot = own_lot(ctx, product, o.out, holding);
    if (lot.whole)
        throw std::runtime_error("работа уже отпущена целиком — продать её ещё и порциями "
                                 "значило бы продать дважды (records.md §9.5)");

    records::LotLink link{};
    link.out = o.out;
    const records::ObservedLotLink*   newest = nullptr;
    std::set<std::array<uint8_t, 32>> returned;   // by earlier own links
    for (const auto& l : lot.links) {
        if (!newest || l.link.seq > newest->link.seq) newest = &l;
        for (const auto& r : l.link.returns) returned.insert(r.hash);
    }
    double                            before = 0.0;
    std::set<std::array<uint8_t, 32>> back_now;   // returned by this very link
    if (newest) {
        link.seq  = newest->link.seq + 1;
        link.prev = Ref{ctx.user_id.bytes, newest->hash};
        before    = newest->link.after;
        std::set<std::array<uint8_t, 32>> taken;
        bool known = false;
        if (const auto st = fetch_work_status(via, product); st && st->is_object()) {
            const auto* k = records::json::find(st->object, "known");
            known = k && k->boolean;
            if (const auto* rs = jarr(st->object, "releases"))
                for (const auto& r : rs->array) {
                    if (!r.is_object()) continue;
                    const auto* t = records::json::find(r.object, "taken");
                    if (t && t->boolean) taken.insert(parse_ref(jstr(r.object, "ref")).hash);
                }
        }
        double back = 0.0;
        if (known)
            for (const auto& l : lot.links)
                if (l.expires && *l.expires <= now && !taken.count(l.hash) &&
                    !returned.count(l.hash)) {
                    link.returns.push_back(Ref{ctx.user_id.bytes, l.hash});
                    back_now.insert(l.hash);
                    back += l.link.qty;
                }
        std::sort(link.returns.begin(), link.returns.end(), [](const Ref& x, const Ref& y) {
            return std::tie(x.chain, x.hash) < std::tie(y.chain, y.hash);
        });
        if (back > 0.0)
            std::cerr << "партия: в запас вернулось " << qty_str(back) << " " << o.unit
                      << " — отпуски истекли, их никто не взял\n";
        before -= back;
    }

    if (numbered) {
        // Номер продаётся один раз (records.md §9.8 v3): он занят звеном, которое
        // его взяло, пока то не вернули.
        std::set<std::string> held;
        for (const auto& l : lot.links)
            if (!returned.count(l.hash) && !back_now.count(l.hash))
                for (const auto& u : l.link.units) held.insert(u);
        if (want.empty()) {
            if (!qty || !(*qty > 0.0) || *qty != std::floor(*qty))
                throw std::runtime_error("партия с номерами: назовите номера (--units) или "
                                         "целое количество (--qty)");
            for (const auto& u : o.units)
                if (want.size() < static_cast<std::size_t>(*qty) && !held.count(u))
                    want.push_back(u);
            if (want.size() < static_cast<std::size_t>(*qty))
                throw std::runtime_error("в партии «" + o.resource + "» свободно номеров: " +
                                         std::to_string(want.size()) +
                                         " — больше не отпустить (records.md §9.8)");
        } else {
            for (const auto& u : want) {
                if (std::find(o.units.begin(), o.units.end(), u) == o.units.end())
                    throw std::runtime_error("в партии «" + o.resource + "» нет номера " + u);
                if (held.count(u))
                    throw std::runtime_error("номер " + u + " уже отпущен или продан — второй "
                                             "раз его не продать (records.md §9.8)");
            }
        }
        std::sort(want.begin(), want.end());
        want.erase(std::unique(want.begin(), want.end()), want.end());
        if (qty && *qty != static_cast<double>(want.size()))
            throw std::runtime_error("--qty не совпадает с числом номеров --units");
        link.units = want;
        link.qty   = static_cast<double>(want.size());
    } else {
        if (!qty || !(*qty > 0.0))
            throw std::runtime_error("--qty: порция должна быть больше нуля");
        link.qty = *qty;
    }
    link.after = before + link.qty;
    if (link.after > o.qty + 1e-9)
        throw std::runtime_error("в партии «" + o.resource + "» осталось " +
                                 qty_str(o.qty - before) + " " + o.unit +
                                 " — больше не отпустить и не списать (records.md §9.8)");
    return link;
}

// The next link of a lot of the own work (records.md §9.8 v3). A portion for
// sale needs the lot's price tag and the work's proposed breakdown: it is sold
// on them.
static records::LotLink next_lot_link(Context& ctx, const std::string& via, const Ref& work,
                                      const WorkRecord& wr, uint32_t out,
                                      std::optional<double> qty,
                                      std::vector<std::string> want, bool for_sale,
                                      int64_t now) {
    if (out >= wr.outputs.size())
        throw std::runtime_error("у работы нет выхода №" + std::to_string(out) +
                                 " — нет и партии (records.md §9.4)");
    const records::ResourceQty& o = wr.outputs[out];
    if (wr.outputs.size() > 1 && !o.share)
        throw std::runtime_error("у работы несколько выходов, а долей часов нет — порцию не "
                                 "оценить; объявите их при записи работы: --output "
                                 "ИМЯ:КОЛ:ЕД:ДОЛЯ (records.md §9.4)");
    if (for_sale && (!o.price || wr.axes.empty()))
        throw std::runtime_error("порции продаются по ценнику, «бери или нет» "
                                 "(records.md §9.8): у партии нет ценника или у работы — "
                                 "предложенной разбивки; назначьте их при записи работы: "
                                 "--output ИМЯ:КОЛ:ЕД::k=K и --axis-share");
    return take_from_lot(ctx, via, work, std::nullopt,
                         LotView{out, o.qty, o.units, o.resource, o.unit}, qty,
                         std::move(want), now);
}

// --buyback | --buyback-to ЦЕПЬ, --buyback-days N | --buyback-until TS: an
// addressed buyout (records.md §9.8 v5) — until the term the buyer sells it on
// only to the addressee and pre-signs that return in its purchase. Plain
// --buyback names oneself: storage, repair. --rent — the same to oneself, but the
// thing is used: it comes back at what is left after the wear by the norm, and
// nothing above the wear is paid (records.md §9.8 v6).
static std::optional<records::Addressed> addressed_from_flags(Context& ctx, int argc,
                                                              char** argv, int64_t now) {
    const auto to_s = flag_val(argc, argv, "--buyback-to");
    const bool rent = flag_present(argc, argv, "--rent");
    if (to_s.empty() && !rent && !flag_present(argc, argv, "--buyback")) return std::nullopt;
    if (rent && !to_s.empty())
        throw std::runtime_error("аренда возвращается тому, кто сдаёт, — --rent без "
                                 "--buyback-to (records.md §9.8)");
    records::Addressed ad{};
    ad.to   = to_s.empty() ? ctx.user_id.bytes : uid_from_hex(to_s);
    ad.wear = rent;
    const auto until_s = flag_val(argc, argv, "--buyback-until");
    ad.until = !until_s.empty()
        ? static_cast<int64_t>(std::stoll(until_s))
        : now + static_cast<int64_t>(
              std::stod(flag_val(argc, argv, "--buyback-days", "30")) * 86400.0);
    return ad;
}

// A rent is paid by the wear, and the wear is counted by the service life the
// maker declared (records.md §9.4 v7): without one there is nothing to count by.
static void rent_needs_life(const records::Release& rl, const WorkRecord& w,
                            std::optional<uint32_t> out) {
    if (!rl.addressed || !rl.addressed->wear) return;
    bool life = false;
    for (uint32_t i = 0; i < w.outputs.size(); ++i)
        if ((!out || *out == i) && w.outputs[i].life) life = true;
    if (!life)
        throw std::runtime_error("у вещи не объявлен срок службы — износа не посчитать, а платы "
                                 "сверх износа нет (records.md §9.8). Срок объявляет мастер при "
                                 "изготовлении (--output …:СРОК); на хранение и в ремонт, без "
                                 "износа, — --buyback");
}

// Which lot of a purchase: the one there is, or the output named by --out.
static const records::HoldingLot& pick_holding_lot(const std::vector<records::HoldingLot>& lots,
                                                   const std::string& out_s) {
    if (out_s.empty()) {
        if (lots.size() != 1)
            throw std::runtime_error("в покупке несколько партий — выберите выход: --out N");
        return lots.front();
    }
    const auto out = static_cast<uint32_t>(std::stoul(out_s));
    for (const auto& l : lots)
        if (l.out == out) return l;
    throw std::runtime_error("в покупке нет партии выхода №" + out_s);
}

// bc release WORK --to CHAIN [--qty Q [--out N]] [--days N | --until TS] [--via URL]
// Отпуск (records.md §9.8, ИР-023): the seller gives the result of an own work —
// whole, or a portion of one of its lots — to a named buyer. Only that buyer's
// deal completes it, and only before the term; without it (or a customer on the
// work itself) a deal on the work is void.
static int cmd_release(const fs::path& data_dir, int argc, char** argv) {
    const auto pos  = get_positionals(argc, argv);
    const auto to_s = flag_val(argc, argv, "--to");
    if (pos.size() < 2 || to_s.empty()) {
        std::cerr << "Usage: bc release РАБОТА --to ЦЕПЬ [--qty КОЛИЧЕСТВО | --units НОМЕР,...]\n"
                     "                  [--out N] [--days N | --until UNIX_TS] [--via URL]\n"
                     "  отдать результат своей работы покупателю (records.md §9.8): засчитана\n"
                     "  будет только его сделка и только до срока (по умолчанию 7 дней).\n"
                     "  --qty / --units — порция партии выхода N (по умолчанию 0), звено её\n"
                     "  подниточки; продаётся по ценнику, «бери или нет»; больше, чем в партии\n"
                     "  осталось, не отпустить, номер — не продать дважды. РАБОТА — хеш своей\n"
                     "  записи труда или ЦЕПЬ/ХЕШ; своя покупка вместо работы — перепродажа\n"
                     "  по цене покупки, без наценки (records.md §9.8)\n";
        return 1;
    }
    const auto      via  = flag_val(argc, argv, "--via");
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    const Ref  work = own_work_arg(ctx, pos[1]);
    const auto wrec = load_record(ctx, "", work);

    records::Release rl{};
    rl.work      = work;
    rl.to        = uid_from_hex(to_s);
    rl.timestamp = static_cast<int64_t>(std::time(nullptr));
    if (rl.to == ctx.user_id.bytes)
        throw std::runtime_error("отпуск себе ничего не отпускает");
    const auto until_s = flag_val(argc, argv, "--until");
    rl.expires = !until_s.empty()
        ? static_cast<int64_t>(std::stoll(until_s))
        : rl.timestamp + static_cast<int64_t>(
              std::stod(flag_val(argc, argv, "--days", "7")) * 86400.0);

    rl.addressed = addressed_from_flags(ctx, argc, argv, rl.timestamp);
    if (rl.addressed) {
        if (rl.addressed->until <= rl.expires)
            throw std::runtime_error("срок возврата не позже срока отпуска — выкупать было бы "
                                     "некогда (records.md §9.8)");
        if (rl.addressed->wear)
            std::cerr << "аренда: до " << rl.addressed->until << " вещь у арендатора; вернётся "
                         "по остатку после износа по норме срока службы — платы сверх износа "
                         "нет (records.md §9.8)\n";
        else
            std::cerr << "выкуп с адресом: до " << rl.addressed->until << " покупатель продаст "
                         "это только " << short_hex(rl.addressed->to) << " — и подпишет возврат "
                         "в самой покупке (records.md §9.8)\n";
    }

    const auto qty_s   = flag_val(argc, argv, "--qty");
    const auto units_s = flag_val(argc, argv, "--units");

    // Перепродажа (records.md §9.8 v4): источник — своя покупка. Серийник остаётся
    // прежним — отпуск называет запись рождения вещи, а цена — доля уплаченного
    // за покупку, без наценки.
    if (const auto* bought = wrec ? std::get_if<records::Transfer>(&*wrec) : nullptr;
        bought && bought->accept) {
        const auto h = load_holding(ctx, via, work);
        if (!h || h->lots.empty())
            throw std::runtime_error("в этой покупке нечего перепродать: купленное — не партия "
                                     "(услуга) или записей продавца не найти — нужен --via");
        if (qty_s.empty() && units_s.empty())
            throw std::runtime_error("купленное перепродают порцией: --qty или --units "
                                     "(records.md §9.8)");
        // Покупка с адресом (records.md §9.8 v5): до срока её продают только
        // адресату, по подписанному возврату, и выкупает он сам; выкупил — продавать
        // нечего.
        if (const auto& b = bought->accept->buyback) {
            if (rl.timestamp < b->until)
                throw std::runtime_error("покупка с адресом: до " + std::to_string(b->until) +
                                         " её продают только " + short_hex(b->to) +
                                         " — по подписанному возврату, выкупает он сам "
                                         "(records.md §9.8)");
            const std::string mine = to_hex(work.chain) + "/" + to_hex(work.hash);
            if (const auto st = fetch_work_status(via, bought->accept->work); st && st->is_object())
                if (const auto* bbs = jarr(st->object, "buybacks"))
                    for (const auto& x : bbs->array)
                        if (x.is_object() && jstr(x.object, "ref") == mine)
                            if (const auto* tk = records::json::find(x.object, "taken");
                                tk && tk->boolean)
                                throw std::runtime_error("покупку уже выкупил адресат — продавать "
                                                         "нечего (records.md §9.8)");
        }
        const auto& hl = pick_holding_lot(h->lots, flag_val(argc, argv, "--out"));
        const auto& o  = h->work.outputs[hl.out];
        rent_needs_life(rl, h->work, hl.out);
        rl.work    = bought->accept->work;
        rl.holding = work;
        rl.lot     = take_from_lot(ctx, via, rl.work, work,
                                   LotView{hl.out, hl.qty, hl.units, o.resource, o.unit},
                                   qty_s.empty() ? std::optional<double>{} : std::stod(qty_s),
                                   split_list(units_s), rl.timestamp);
        const Block block = append_record(ctx, Record{rl});
        std::cout << "release ref: " << to_hex(ctx.user_id.bytes) << "/"
                  << to_hex(Crypto::hash_block(block).bytes) << "\n";
        std::cerr << "перепродажа: " << qty_str(rl.lot->qty) << " " << o.unit << " «"
                  << o.resource << "»";
        for (std::size_t i = 0; i < rl.lot->units.size(); ++i)
            std::cerr << (i ? ", " : " (") << rl.lot->units[i]
                      << (i + 1 == rl.lot->units.size() ? ")" : "");
        std::cerr << " → " << short_hex(rl.to) << " по цене покупки: "
                  << records::resale_price(hl, rl.lot->qty) << " стч, без наценки; из купленных "
                  << qty_str(hl.qty) << " осталось " << qty_str(hl.qty - rl.lot->after)
                  << ", срок " << rl.expires << "\n";
        if (!via.empty()) upload_block(via, block);
        return 0;
    }
    const WorkRecord* wr = wrec ? std::get_if<WorkRecord>(&*wrec) : nullptr;
    if (!wr) throw std::runtime_error("своей записи труда или покупки с таким хешем нет");

    if (!qty_s.empty() || !units_s.empty()) {
        // Порция партии (records.md §9.8 v3): звено подниточки — после последнего.
        if (wr->customer)
            throw std::runtime_error("работа на заказ — результат целиком отпущен заказчику "
                                     "(records.md §9.4); порциями продают работу на склад");
        const auto out = static_cast<uint32_t>(std::stoul(flag_val(argc, argv, "--out", "0")));
        rl.lot = next_lot_link(ctx, via, work, *wr, out,
                               qty_s.empty() ? std::optional<double>{} : std::stod(qty_s),
                               split_list(units_s), /*for_sale=*/true, rl.timestamp);
    } else {
        if (wr->customer && *wr->customer == rl.to) {
            std::cerr << "работа и так на заказ этой цепи — результат отпущен ей самой "
                         "записью труда (records.md §9.4), отпуск не нужен\n";
            return 0;
        }
        // Один результат — одна сделка (records.md §9.5): не отпускаем проданное,
        // не отпускаем второму, пока жив отпуск первому, и не отпускаем целиком
        // то, что уже идёт порциями, — это была бы двойная продажа, и подписана
        // она была бы нами.
        for (const NodeIndex node : ctx.local_branches()) {
            std::vector<Block> blocks;
            try { blocks = ctx.bc.get_branch(ctx.user_id, node); }
            catch (const BlockchainError&) { continue; }
            for (const auto& b : blocks) {
                if (b.type != BlockType::DATA) continue;
                Record rec;
                try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
                catch (const CodecError&) { continue; }
                if (const auto* prev = std::get_if<records::Release>(&rec);
                    prev && prev->work == work) {
                    if (prev->lot)
                        throw std::runtime_error("работа уже продаётся порциями — отпустить её "
                                                 "целиком значило бы продать дважды "
                                                 "(records.md §9.5)");
                    if (prev->to != rl.to && prev->expires > rl.timestamp)
                        throw std::runtime_error(
                            "работа уже отпущена " + short_hex(prev->to) + " до " +
                            std::to_string(prev->expires) + " — второй отпуск до срока "
                            "первого был бы двойной продажей (records.md §9.8)");
                }
                if (const auto* wo = std::get_if<records::WriteOff>(&rec);
                    wo && wo->work == work)
                    throw std::runtime_error("из партий этой работы уже списано — целиком её не "
                                             "отпустить (records.md §9.9)");
            }
        }
        if (const auto st = fetch_work_status(via, work); st && st->is_object())
            if (const auto* ds = jarr(st->object, "deals"))
                for (const auto& d : ds->array)
                    if (d.is_object() && jstr(d.object, "state") != "unaddressed")
                        throw std::runtime_error("работа уже продана: сделка " +
                                                 jstr(d.object, "ref") + " (records.md §9.5)");
        if (wr->customer)
            std::cerr << "⚠ работа была на заказ " << short_hex(*wr->customer)
                      << " — отпуская её другому, вы отдаёте результат второй раз; если "
                         "заказчик тоже заплатит, это двойная продажа (records.md §9.5)\n";
    }

    rent_needs_life(rl, *wr, rl.lot ? std::optional<uint32_t>(rl.lot->out) : std::nullopt);
    const Block block = append_record(ctx, Record{rl});
    std::cout << "release ref: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    if (rl.lot) {
        const auto& o = wr->outputs[rl.lot->out];
        std::cerr << "отпуск порции: " << qty_str(rl.lot->qty) << " " << o.unit << " «"
                  << o.resource << "»";
        for (std::size_t i = 0; i < rl.lot->units.size(); ++i)
            std::cerr << (i ? ", " : " (") << rl.lot->units[i]
                      << (i + 1 == rl.lot->units.size() ? ")" : "");
        std::cerr << " → " << short_hex(rl.to) << ", в партии осталось "
                  << qty_str(o.qty - rl.lot->after) << " " << o.unit;
    } else {
        std::cerr << "отпуск: результат работы " << short_hex(work.hash) << " → "
                  << short_hex(rl.to);
    }
    std::cerr << ", срок " << rl.expires << " (через "
              << (rl.expires - rl.timestamp) / 86400.0 << " дн.)\n";
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc writeoff WORK --qty Q [--out N] --reason TEXT [--via URL] [--leaf L]
// Списание (records.md §9.9): part of a lot leaves it without a sale — spoiled,
// lost, used up — by a link of the lot's sub-thread, so the remainder stays
// checkable and the loss stays visible in the own chain. WORK may be an own
// purchase: bought goods are written off from its lot (v2).
static int cmd_writeoff(const fs::path& data_dir, int argc, char** argv) {
    const auto pos     = get_positionals(argc, argv);
    const auto qty_s   = flag_val(argc, argv, "--qty");
    const auto units_s = flag_val(argc, argv, "--units");
    const auto reason  = flag_val(argc, argv, "--reason");
    if (pos.size() < 2 || (qty_s.empty() && units_s.empty()) || reason.empty()) {
        std::cerr << "Usage: bc writeoff РАБОТА (--qty КОЛИЧЕСТВО | --units НОМЕР,...)\n"
                     "                   --reason ТЕКСТ [--out N] [--via URL] [--leaf L]\n"
                     "  списать часть партии без продажи — испорчено, потеряно,\n"
                     "  израсходовано (records.md §9.9): звено подниточки партии;\n"
                     "  РАБОТА — своя запись труда или своя покупка\n";
        return 1;
    }
    const auto      via  = flag_val(argc, argv, "--via");
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    const Ref  source = own_work_arg(ctx, pos[1]);
    const auto srec   = load_record(ctx, "", source);
    const auto qty    = qty_s.empty() ? std::optional<double>{} : std::stod(qty_s);

    records::WriteOff wo{};
    wo.reason    = reason;
    wo.timestamp = static_cast<int64_t>(std::time(nullptr));
    std::string resource, unit;
    double      lot_qty = 0.0;
    if (const auto* bought = srec ? std::get_if<records::Transfer>(&*srec) : nullptr;
        bought && bought->accept) {
        // Списание купленного (records.md §9.9 v2): звено подниточки покупки.
        const auto h = load_holding(ctx, via, source);
        if (!h || h->lots.empty())
            throw std::runtime_error("в этой покупке нечего списывать: купленное — не партия "
                                     "или записей продавца не найти — нужен --via");
        const auto& hl = pick_holding_lot(h->lots, flag_val(argc, argv, "--out"));
        const auto& o  = h->work.outputs[hl.out];
        wo.work    = bought->accept->work;
        wo.holding = source;
        wo.lot     = take_from_lot(ctx, via, wo.work, source,
                                   LotView{hl.out, hl.qty, hl.units, o.resource, o.unit}, qty,
                                   split_list(units_s), wo.timestamp);
        resource = o.resource;
        unit     = o.unit;
        lot_qty  = hl.qty;
    } else {
        const WorkRecord* wr = srec ? std::get_if<WorkRecord>(&*srec) : nullptr;
        if (!wr) throw std::runtime_error("своей записи труда или покупки с таким хешем нет");
        if (wr->customer)
            throw std::runtime_error("работа на заказ — её результат у заказчика, из своего "
                                     "запаса списывать нечего (records.md §9.9)");
        const auto out = static_cast<uint32_t>(std::stoul(flag_val(argc, argv, "--out", "0")));
        wo.work = source;
        wo.lot  = next_lot_link(ctx, via, source, *wr, out, qty, split_list(units_s),
                                /*for_sale=*/false, wo.timestamp);
        resource = wr->outputs[out].resource;
        unit     = wr->outputs[out].unit;
        lot_qty  = wr->outputs[out].qty;
    }

    const Block block = append_record(ctx, Record{wo});
    std::cout << "writeoff ref: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    std::cerr << "списано: " << qty_str(wo.lot.qty) << " " << unit << " «" << resource
              << "» — " << reason << "; в партии осталось " << qty_str(lot_qty - wo.lot.after)
              << " " << unit << "\n";
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc thread [ЦЕПЬ] --via URL
// The goods thread of a chain (records.md §9.4 v4, ИР-023): its works in time,
// the faults — outside the thread, broken links, hours sold twice — and the
// labor no deal has confirmed yet, by the axes the worker proposed.
static int cmd_thread(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc thread [ЦЕПЬ] --via URL   (по умолчанию — своя цепь)\n";
        return 1;
    }
    const std::string chain = pos.size() >= 2 ? pos[1] : to_hex(load_user_id(data_dir).bytes);
    const auto root = fetch_json(via, "/thread/" + chain);
    if (!root || !root->is_object()) {
        std::cerr << "агрегатор не знает работ этой цепи (блоки выгружены — --via?)\n";
        return 1;
    }
    const auto& o = root->object;
    const auto when = [](double ts) {
        const std::time_t t = static_cast<std::time_t>(ts);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::gmtime(&t));
        return std::string(buf);
    };
    const auto* works = jarr(o, "works");
    const auto* gaps  = records::json::find(o, "gaps");
    std::cout << "нить труда " << chain.substr(0, 16) << "…  (records.md §9.4; время UTC)\n"
              << "  работ: " << (works ? works->array.size() : 0)
              << ", форков: " << static_cast<int>(jnum(o, "forks"))
              << (gaps && gaps->boolean ? ", есть пробелы — не все звенья у агрегатора" : "")
              << "\n";
    if (works)
        for (const auto& w : works->array) {
            if (!w.is_object()) continue;
            const auto& x     = w.object;
            const auto* sold  = records::json::find(x, "sold");
            const auto  fault = jstr(x, "fault");
            std::cout << "  " << when(jnum(x, "start")) << "  " << jnum(x, "hours") << "ч  "
                      << jstr(x, "action")
                      << (sold && sold->boolean ? "  [продана]" : "")
                      << (fault == "unthreaded" ? "  ⚠ вне нити"
                        : fault == "broken"     ? "  ⚠ битое звено"
                        : fault == "crossing"   ? "  ✗ часы заняты другой работой" : "")
                      << "\n";
            if (const auto* lots = jarr(x, "lots"))       // records.md §9.8 v2
                for (const auto& l : lots->array) {
                    if (!l.is_object()) continue;
                    const auto& y = l.object;
                    std::cout << "      партия «" << jstr(y, "resource") << "» " << jnum(y, "qty")
                              << " " << jstr(y, "unit") << ": продано " << jnum(y, "sold")
                              << ", ждёт оплаты " << jnum(y, "reserved")
                              << ", списано " << jnum(y, "written_off");
                    if (jnum(y, "consumed") > 0)
                        std::cout << ", в производство " << jnum(y, "consumed");
                    std::cout << ", в запасе " << jnum(y, "remaining");
                    if (jnum(y, "faults") > 0)
                        std::cout << "  ✗ звеньев не сходится: " << jnum(y, "faults");
                    if (const auto* fu = jarr(y, "free_units"); fu && !fu->array.empty()) {
                        std::cout << "; свободны номера:";
                        for (std::size_t i = 0; i < fu->array.size() && i < 8; ++i)
                            std::cout << " " << fu->array[i].string;
                        if (fu->array.size() > 8) std::cout << " …";
                    }
                    std::cout << "\n";
                }
        }
    if (const auto* ov = jarr(o, "overlaps"); ov && !ov->array.empty()) {
        std::cout << "  одни и те же часы в двух работах — подписано самой цепью:\n";
        for (const auto& p : ov->array)
            if (p.is_array() && p.array.size() == 2)
                std::cout << "    " << p.array[0].string.substr(p.array[0].string.find('/') + 1, 12)
                          << " ⟷ "   << p.array[1].string.substr(p.array[1].string.find('/') + 1, 12)
                          << "\n";
    }
    if (const auto* un = records::json::find(o, "unconfirmed"); un && un->is_object()) {
        std::cout << "  ещё не подтверждено сделкой: " << jnum(un->object, "hours") << "ч";
        if (const auto* ax = jarr(un->object, "axes"); ax && !ax->array.empty()) {
            std::cout << " — по осям:";
            for (const auto& a : ax->array) {
                if (!a.is_object()) continue;
                const auto label = jstr(a.object, "label");
                std::cout << "  " << (label.empty() ? jstr(a.object, "axis").substr(0, 12) : label)
                          << " " << jnum(a.object, "hours") << "ч";
            }
        }
        if (jnum(un->object, "without_axes") > 0)
            std::cout << "  (без разбивки: " << jnum(un->object, "without_axes") << "ч)";
        std::cout << "\n";
    }
    return 0;
}

// bc stock [ЦЕПЬ] --via URL
// What a chain bought and may sell on (records.md §9.8 v4): each purchase of
// goods with its lots — sold on, written off, waiting for payment, in stock —
// and what it cost, the price it is sold on at.
static int cmd_stock(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc stock [ЦЕПЬ] --via URL   (по умолчанию — своя цепь)\n";
        return 1;
    }
    const std::string chain = pos.size() >= 2 ? pos[1] : to_hex(load_user_id(data_dir).bytes);
    const auto root = fetch_json(via, "/stock/" + chain);
    if (!root || !root->is_object()) {
        std::cerr << "агрегатор не ответил\n";
        return 1;
    }
    const auto* holdings = jarr(root->object, "holdings");
    std::cout << "купленное " << chain.substr(0, 16) << "…  (records.md §9.8)\n";
    if (!holdings || holdings->array.empty()) {
        std::cout << "  купленных партий нет\n";
        return 0;
    }
    for (const auto& hv : holdings->array) {
        if (!hv.is_object()) continue;
        const auto& h   = hv.object;
        const auto  ref = jstr(h, "ref");
        std::cout << "  «" << jstr(h, "action") << "» — покупка "
                  << ref.substr(ref.find('/') + 1, 12) << "\n";
        if (const auto* b = records::json::find(h, "buyback"); b && b->is_object()) {
            const auto* tk   = records::json::find(b->object, "taken");
            const auto* we   = records::json::find(b->object, "wear");
            const bool  rent = we && we->boolean;
            std::cout << "      " << (rent ? "аренда" : "выкуп с адресом") << ": вернуть "
                      << jstr(b->object, "to").substr(0, 16) << "… до "
                      << static_cast<int64_t>(jnum(b->object, "until"))
                      << (rent ? " по остатку после износа" : " по цене покупки")
                      << (tk && tk->boolean ? " — выкуплено" : "") << "\n";
        }
        if (const auto* lots = jarr(h, "lots"))
            for (const auto& lv : lots->array) {
                if (!lv.is_object()) continue;
                const auto& y = lv.object;
                std::cout << "      «" << jstr(y, "resource") << "» " << jnum(y, "qty") << " "
                          << jstr(y, "unit") << " за " << jnum(y, "cost") << " стч: перепродано "
                          << jnum(y, "sold") << ", ждёт оплаты " << jnum(y, "reserved")
                          << ", списано " << jnum(y, "written_off");
                if (jnum(y, "consumed") > 0)
                    std::cout << ", в производство " << jnum(y, "consumed");
                std::cout << ", в запасе " << jnum(y, "remaining");
                if (const auto* fu = jarr(y, "free_units"); fu && !fu->array.empty()) {
                    std::cout << "; номера:";
                    for (std::size_t i = 0; i < fu->array.size() && i < 8; ++i)
                        std::cout << " " << fu->array[i].string;
                }
                std::cout << "\n";
            }
    }
    return 0;
}

// ── bc tool — средства производства (ИР-011, records.md §10.2) ────────────────

// Купленное — в своё производство (records.md §10.1 v3, ИР-023): запись материала
// или средства производства заводится звеном подниточки своей покупки. Ни
// стоимости, ни размера не объявляют: стоимость — доля уплаченного за взятое (как
// при перепродаже), размер — само взятое. Что ушло в производство, то уже не
// продать: звено считается в той же подниточке, что отпуски и списания.
struct TakenFromPurchase {
    records::LotLink lot;
    double           cost = 0;
    double           qty  = 0;
    std::string      unit;
    std::string      resource;
};

static TakenFromPurchase take_into_production(Context& ctx, const std::string& via,
                                              const Ref& purchase, int argc, char** argv,
                                              std::optional<double> qty,
                                              std::vector<std::string> units, int64_t now) {
    if (purchase.chain != ctx.user_id.bytes)
        throw std::runtime_error("--paid: в своё производство заводят свою покупку");
    const auto h = load_holding(ctx, via, purchase);
    if (!h || h->lots.empty())
        throw std::runtime_error("--paid: своей покупки не прочесть или в ней нет партии — "
                                 "нужен --via (records.md §10.1)");
    const auto& hl = pick_holding_lot(h->lots, flag_val(argc, argv, "--out"));
    const auto& o  = h->work.outputs[hl.out];
    TakenFromPurchase t{};
    t.lot = take_from_lot(ctx, via, h->deal.accept->work, purchase,
                          LotView{hl.out, hl.qty, hl.units, o.resource, o.unit},
                          qty, std::move(units), now);
    t.cost     = records::resale_price(hl, t.lot.qty);
    t.qty      = t.lot.qty;
    t.unit     = o.unit;
    t.resource = o.resource;
    std::cerr << "из своей покупки " << short_hex(purchase.hash) << ": " << qty_str(t.qty)
              << " " << t.unit << " «" << t.resource << "» за " << t.cost
              << "ч — доля уплаченного, без наценки; из купленных " << qty_str(hl.qty)
              << " осталось " << qty_str(hl.qty - t.lot.after)
              << " (records.md §10.1)\n";
    return t;
}

// bc tool add ИМЯ --cost H --life HOURS [--serial S] [--desc T] [--note T]
//             [--paid ACC_REF] [--origin TOOL_REF] [--leaf L] [--via URL]
static int cmd_tool_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos    = get_positionals(argc, argv);
    const auto cost_s = flag_val(argc, argv, "--cost");
    const auto life_s = flag_val(argc, argv, "--life");
    const auto paid_s = flag_val(argc, argv, "--paid");
    if (pos.size() < 3 || life_s.empty() || (paid_s.empty() && cost_s.empty())) {
        std::cerr <<
            "Usage: bc tool add ИМЯ --cost ТРУДОЧАСЫ --life ЧАСЫ_РЕСУРСА\n"
            "   [--serial S] [--desc TEXT] [--note TEXT]\n"
            "   [--paid ПОКУПКА]      взять из своей покупки: --paid ССЫЛКА\n"
            "                         [--out N] [--units НОМЕР] — стоимость из сделки\n"
            "   [--origin TOOL_REF]   перевыпуск: перепродажа / переоценка вниз /\n"
            "                         повторный ввод (cost ≤ остатка предыдущей)\n"
            "   [--leaf L] [--via URL]\n"
            "Без --paid стоимость — оценка владельца (est): дайте --note с расчётом\n"
            "(например: \"120000₽ / 380₽·ч ≈ 315ч\").\n";
        return 1;
    }
    records::Tool t{};
    t.name   = pos[2];
    t.desc   = flag_val(argc, argv, "--desc");
    t.serial = flag_val(argc, argv, "--serial");
    t.life   = std::stod(life_s);
    t.note   = flag_val(argc, argv, "--note");
    const auto origin_s = flag_val(argc, argv, "--origin");
    t.basis = paid_s.empty() ? "est" : "paid";
    if (!origin_s.empty()) t.origin = parse_ref(origin_s);

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    if (!paid_s.empty()) {
        // Куплен в системе (records.md §10.2 v3): стоимость — доля уплаченного за
        // взятое, звеном подниточки своей покупки; ресурс `life` оценивает хозяин.
        if (!cost_s.empty())
            throw std::runtime_error("--cost с --paid не задают: стоимость — доля уплаченного "
                                     "за взятое (records.md §10.2)");
        t.src = parse_ref(paid_s);
        const auto units = split_list(flag_val(argc, argv, "--units"));
        const auto taken = take_into_production(
            ctx, flag_val(argc, argv, "--via"), *t.src, argc, argv,
            units.empty() ? std::optional<double>{1.0} : std::optional<double>{},
            units, static_cast<int64_t>(std::time(nullptr)));
        t.lot  = taken.lot;
        t.cost = taken.cost;
        if (t.serial.empty() && !taken.lot.units.empty()) t.serial = taken.lot.units.front();
    } else {
        t.cost = std::stod(cost_s);
    }
    if (t.cost <= 0 || t.life <= 0)
        throw std::runtime_error("--cost и --life должны быть положительными");
    if (t.basis == "est" && t.note.empty())
        std::cerr << "предупреждение: est без --note — покупателю не из чего "
                     "проверить оценку\n";

    // Reissue ceiling (records.md §10.2): new cost ≤ previous remainder.
    // Enforceable locally only for an own origin; a foreign seller's thread
    // is checked by whoever audits the chains.
    if (t.origin) {
        if (t.origin->chain == ctx.user_id.bytes) {
            const auto prev = find_own_assets(ctx, to_hex(t.origin->hash));
            if (prev.empty())
                throw std::runtime_error("--origin не найден в своей цепи");
            const auto   st        = carry_thread_state(ctx, prev[0].ref);
            const double remainder = prev[0].info.cost - st.collected;
            if (t.cost > remainder + 1e-9) {
                std::cerr << "отказ: cost " << t.cost << "ч выше остатка "
                          << remainder << "ч предыдущей записи — перевыпуск "
                          "только вниз (records.md §10.2)\n";
                return 1;
            }
        } else {
            std::cerr << "предупреждение: origin в чужой цепи — остаток "
                         "продавца локально не проверен\n";
        }
    }

    const Block block = append_record(ctx, Record{t});
    std::cout << "tool ref: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    std::cerr << "  " << t.name << "  " << t.cost << "ч, ресурс " << t.life
              << "ч  [" << (t.basis == "paid" ? "куплен в системе"
                                              : "оценка владельца") << "]\n"
              << "  использование: bc work log ... --tool ПРЕФИКС_ХЕША:ЧАСЫ\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc material add ИМЯ --unit U --cost H --qty Q [--desc T] [--note T]
//                 [--paid ACC_REF] [--origin MAT_REF] [--leaf L] [--via URL]
// A batch of consumables (records.md §10.1 v2): cost flows into products as
// the quantity is spent — the same carry thread as a tool.
static int cmd_material_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos    = get_positionals(argc, argv);
    const auto unit_s = flag_val(argc, argv, "--unit");
    const auto cost_s = flag_val(argc, argv, "--cost");
    const auto qty_s  = flag_val(argc, argv, "--qty");
    const auto paid_s = flag_val(argc, argv, "--paid");
    if (pos.size() < 3 || (paid_s.empty() && (unit_s.empty() || cost_s.empty() || qty_s.empty()))
        || (!paid_s.empty() && qty_s.empty())) {
        std::cerr <<
            "Usage: bc material add ИМЯ --unit ЕД --cost ТРУДОЧАСЫ --qty РАЗМЕР\n"
            "   [--desc TEXT] [--note TEXT]\n"
            "   [--paid ПОКУПКА]      взять из своей покупки: --paid ССЫЛКА --qty КОЛ\n"
            "                         [--out N] — стоимость и размер из самой сделки\n"
            "   [--origin MAT_REF]    перевыпуск (cost ≤ остатка предыдущей)\n"
            "   [--leaf L] [--via URL]\n"
            "Партия расходуется в работе флагом: bc work log ... "
            "--material ПРЕФИКС_ХЕША:КОЛИЧЕСТВО\n"
            "Без --paid стоимость — оценка (est): дайте --note (например \"чек "
            "2100₽ / 525₽·ч\").\n";
        return 1;
    }
    records::Material m{};
    m.name = pos[2];
    m.unit = unit_s;
    m.desc = flag_val(argc, argv, "--desc");
    m.note = flag_val(argc, argv, "--note");
    const auto origin_s = flag_val(argc, argv, "--origin");
    m.basis = paid_s.empty() ? "est" : "paid";
    if (!origin_s.empty()) m.origin = parse_ref(origin_s);

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    if (!paid_s.empty()) {
        // Купленное — в своё производство (records.md §10.1 v3): числа из сделки.
        if (!cost_s.empty())
            throw std::runtime_error("--cost с --paid не задают: стоимость — доля уплаченного "
                                     "за взятое (records.md §10.1)");
        m.src = parse_ref(paid_s);
        const auto t = take_into_production(ctx, flag_val(argc, argv, "--via"), *m.src,
                                            argc, argv, std::stod(qty_s), {},
                                            static_cast<int64_t>(std::time(nullptr)));
        m.lot  = t.lot;
        m.cost = t.cost;
        m.qty  = t.qty;
        if (m.unit.empty()) m.unit = t.unit;
    } else {
        m.cost = std::stod(cost_s);
        m.qty  = std::stod(qty_s);
    }
    if (m.cost <= 0 || m.qty <= 0)
        throw std::runtime_error("--cost и --qty должны быть положительными");
    if (m.basis == "est" && m.note.empty())
        std::cerr << "предупреждение: est без --note — покупателю не из чего "
                     "проверить оценку\n";

    // Reissue ceiling (records.md §10.2): new cost ≤ previous remainder —
    // enforceable locally only for an own origin.
    if (m.origin && m.origin->chain == ctx.user_id.bytes) {
        const auto prev = find_own_assets(ctx, to_hex(m.origin->hash));
        if (prev.empty())
            throw std::runtime_error("--origin не найден в своей цепи");
        const auto   st        = carry_thread_state(ctx, prev[0].ref);
        const double remainder = prev[0].info.cost - st.collected;
        if (m.cost > remainder + 1e-9) {
            std::cerr << "отказ: cost " << m.cost << "ч выше остатка "
                      << remainder << "ч предыдущей записи (records.md §10.2)\n";
            return 1;
        }
    }

    const Block block = append_record(ctx, Record{m});
    std::cout << "material ref: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    std::cerr << "  " << m.name << "  " << m.cost << "ч / партия " << m.qty
              << " " << m.unit << "  [" << (m.basis == "paid" ? "куплена"
                                                              : "оценка") << "]\n"
              << "  расход: bc work log ... --material ПРЕФИКС_ХЕША:КОЛИЧЕСТВО\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc tool list [--leaf L] — own means of production with thread state
static int cmd_tool_list(const fs::path& data_dir, int argc, char** argv) {
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    const auto assets = find_own_assets(ctx, "");
    if (assets.empty()) {
        std::cout << "средств производства нет — заведите: bc tool add\n";
        return 0;
    }
    for (const auto& a : assets) {
        const auto   st        = carry_thread_state(ctx, a.ref);
        const double remainder = std::max(0.0, a.info.cost - st.collected);
        std::cout << short_hex(a.ref.hash) << "  " << a.info.name
                  << "  " << a.info.cost << "ч"
                  << "  собрано " << st.collected << "ч"
                  << "  остаток " << remainder << "ч  ["
                  << (a.info.is_material ? "партия " : "ресурс ")
                  << a.info.capacity
                  << (a.info.is_material ? " " + a.info.unit : "ч")
                  << ", " << (a.info.basis == "paid" ? "куплен" : "оценка") << "]\n";
        if (remainder <= 1e-9)
            std::cout << "    стоимость возвращена полностью — дальше работает "
                         "бесплатно (records.md §9.4)\n";
    }
    return 0;
}

// bc tool show HASH_PREFIX [--leaf L] — audit one asset's carry thread
static int cmd_tool_show(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);
    if (pos.size() < 3) {
        std::cerr << "Usage: bc tool show HASH_PREFIX [--leaf L]\n";
        return 1;
    }
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    const auto matches = find_own_assets(ctx, pos[2]);
    if (matches.empty()) { std::cerr << "не найдено: " << pos[2] << "\n"; return 1; }
    if (matches.size() > 1) {
        std::cerr << "префикс неоднозначен:\n";
        for (const auto& m : matches)
            std::cerr << "  " << short_hex(m.ref.hash) << "  " << m.info.name << "\n";
        return 1;
    }
    const auto& a = matches[0];

    // Every observed link of this asset over the local branches → the same
    // digest a counterparty would run (records::carry_history).
    std::vector<records::ObservedCarry> links;
    for (const NodeIndex branch : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, branch); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }
            const auto* wr = std::get_if<WorkRecord>(&rec);
            if (!wr) continue;
            for (const auto& ce : wr->carry)
                if (ce.src == a.ref) {
                    records::ObservedCarry o{};
                    o.entry      = ce;
                    o.timestamp  = wr->start_ts;
                    o.block_hash = Crypto::hash_block(b).bytes;
                    links.push_back(o);
                }
        }
    }
    const auto h = records::carry_history(std::move(links),
                                          a.info.cost, a.info.capacity);

    std::cout << a.info.name << "\n  ref: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(a.ref.hash) << "\n"
              << "  стоимость: " << a.info.cost << "ч  ["
              << (a.info.basis == "paid" ? "куплен в системе"
                                         : "оценка владельца") << "]\n"
              << "  " << (a.info.is_material ? "партия: " : "ресурс: ")
              << a.info.capacity
              << (a.info.is_material ? " " + a.info.unit : "ч") << "\n"
              << "  собрано: " << h.collected
              << "ч   остаток (потолок перепродажи, economy.md §5б): "
              << std::max(0.0, a.info.cost - h.collected) << "ч\n"
              << "  звеньев нити: " << h.links_seen << "/" << h.links_expected
              << (h.gaps ? "  — есть пропуски (картина неполная)" : "") << "\n";

    bool clean = true;
    if (!h.equivocated_seqs.empty()) {
        clean = false;
        std::cout << "  ⚠ ФОРК НИТИ — двойное списание по параллельным веткам, seq:";
        for (const auto s : h.equivocated_seqs) std::cout << " " << s;
        std::cout << "\n";
    }
    if (h.over_invariant) {
        clean = false;
        std::cout << "  ⚠ перенесено больше стоимости — денежный насос (records.md §9.4)\n";
    }
    if (h.formula_mismatch) {
        clean = false;
        std::cout << "  ⚠ есть звено не по формуле переноса (records.md §9.4)\n";
    }
    if (h.after_decreasing) {
        clean = false;
        std::cout << "  ⚠ собранная сумма убывает по нити\n";
    }
    if (h.thread_inconsistent) {
        clean = false;
        std::cout << "  ⚠ разрыв непрерывности между соседними звеньями\n";
    }
    if (clean) std::cout << "  нить чистая\n";
    return 0;
}

// bc merge prepare [--leaf L]
// Builds and prints your BranchTipInfo as a hex blob — send to your merge partner.
static int cmd_merge_prepare(const fs::path& data_dir, int argc, char** argv) {
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    MergeSession session(ctx.storage, ctx.validator);

    const auto tip = session.prepare_tip(ctx.user_id, leaf);
    if (tip.tip_address.block_index == EMPTY_BRANCH_INDEX) {
        std::cerr << "Branch is empty — append a stub block first: bc block stub\n";
        return 1;
    }
    const auto snapshot = session.snapshot_for(ctx.user_id, leaf);

    const auto tip_bytes  = Serializer::encode(tip);
    const auto snap_bytes = Serializer::encode(snapshot);
    std::cout << "tip:      " << to_hex(tip_bytes.data(),  tip_bytes.size())  << "\n";
    std::cout << "snapshot: " << to_hex(snap_bytes.data(), snap_bytes.size()) << "\n";
    std::cerr << "(send both blobs to your merge partner)\n";
    std::cerr << "Next: bc merge create --peer-tip <TIP> --peer-snapshot <SNAP>\n";
    return 0;
}

// bc merge create --peer-tip HEX --peer-snapshot HEX [--leaf L] [--depth N]
// Verifies partner's tip, unions snapshots, creates own MERGE draft, saves state.
// Prints draft_hash to send to partner.
static int cmd_merge_create(const fs::path& data_dir, int argc, char** argv) {
    const auto peer_hex = flag_val(argc, argv, "--peer-tip");
    const auto snap_hex = flag_val(argc, argv, "--peer-snapshot");
    if (peer_hex.empty() || snap_hex.empty()) {
        std::cerr << "Usage: bc merge create --peer-tip HEX --peer-snapshot HEX "
                     "[--leaf L] [--depth N]\n";
        return 1;
    }

    const auto peer_cbor     = from_hex_vec(peer_hex);
    const auto partner_tip   = Serializer::decode_tip(peer_cbor.data(), peer_cbor.size());
    const auto snap_cbor     = from_hex_vec(snap_hex);
    const auto partner_snap  = Serializer::decode_snapshot(snap_cbor.data(), snap_cbor.size());

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    MergeSession session(ctx.storage, ctx.validator);

    session.verify_partner_tip(partner_tip);

    const auto     now   = static_cast<Timestamp>(std::time(nullptr));
    const auto     dep_s = flag_val(argc, argv, "--depth");
    const uint32_t validated_depth =
        dep_s.empty() ? 1u : static_cast<uint32_t>(std::stoul(dep_s));

    // Pre-merge view of the own branch — create_pending replaces the stored
    // snapshot with the union, so capture both for the cache fill (sync.md §5.2).
    const auto own_tip  = session.prepare_tip(ctx.user_id, leaf);
    const auto own_snap = session.snapshot_for(ctx.user_id, leaf);

    const auto pending = session.create_pending(
        ctx.user_id, leaf, partner_tip, partner_snap, ctx.working_kp, now, validated_depth);

    MergeState state{};
    state.partner_pubkey    = partner_tip.path.back().working_pubkey;
    state.draft_block_index = pending.draft.address.block_index;
    state.peer_tip_cbor     = peer_cbor;
    state.own_tip_cbor      = Serializer::encode(own_tip);
    state.own_snap_cbor     = Serializer::encode(own_snap);
    state.peer_snap_cbor    = snap_cbor;
    save_merge_state(data_dir, leaf, state);

    const auto pid = to_hex(partner_tip.path.front().structural_pubkey.bytes);
    std::cerr << "Partner verified: " << pid.substr(0, 16) << "...\n";
    std::cerr << "Draft MERGE block #" << pending.draft.address.block_index << " saved.\n";
    std::cout << "draft_hash: " << to_hex(pending.draft_hash.bytes) << "\n";
    std::cerr << "(send draft_hash to partner; they run: bc merge cosign --draft <HASH>)\n";
    std::cerr << "Next (after receiving partner's co_sig): bc merge finalize --co-sig <SIG>\n";
    return 0;
}

// bc merge cosign --draft DRAFT_HASH_HEX [--leaf L]
// Co-signs the partner's draft hash with own working key. Prints the signature to send back.
static int cmd_merge_cosign(const fs::path& data_dir, int argc, char** argv) {
    const auto draft_hex = flag_val(argc, argv, "--draft");
    if (draft_hex.empty()) {
        std::cerr << "Usage: bc merge cosign --draft DRAFT_HASH_HEX [--leaf L]\n";
        return 1;
    }

    Hash draft_hash{};
    if (!from_hex(draft_hex, draft_hash.bytes.data(), 32)) {
        std::cerr << "Invalid --draft hex (expected 64 hex chars)\n";
        return 1;
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    MergeSession session(ctx.storage, ctx.validator);

    const Signature co_sig = session.co_sign(draft_hash, ctx.working_kp);
    std::cout << "co_signature: " << to_hex(co_sig.bytes) << "\n";
    std::cerr << "(send co_signature to partner; they run: bc merge finalize --co-sig <SIG>)\n";
    return 0;
}

// bc merge finalize --co-sig HEX [--leaf L]
// Attaches partner's co-signature to own pending MERGE block and completes the merge.
static int cmd_merge_finalize(const fs::path& data_dir, int argc, char** argv) {
    const auto cosig_hex = flag_val(argc, argv, "--co-sig");
    if (cosig_hex.empty()) {
        std::cerr << "Usage: bc merge finalize --co-sig HEX [--leaf L]\n";
        return 1;
    }

    Signature co_sig{};
    if (!from_hex(cosig_hex, co_sig.bytes.data(), 64)) {
        std::cerr << "Invalid --co-sig hex (expected 128 hex chars)\n";
        return 1;
    }

    const NodeIndex leaf  = parse_leaf_index(argc, argv);
    const auto state      = load_merge_state(data_dir, leaf);
    Context ctx(data_dir, leaf);

    const Block draft = ctx.storage.get_block({ctx.user_id, leaf, state.draft_block_index});
    const PendingMergeBlock pending{draft, Crypto::hash_block(draft)};

    MergeSession session(ctx.storage, ctx.validator);
    const Block finalized = session.finalize(pending, co_sig, state.partner_pubkey);

    if (!state.peer_tip_cbor.empty()) {
        const auto partner_tip = Serializer::decode_tip(
            state.peer_tip_cbor.data(), state.peer_tip_cbor.size());
        session.import_partner_data(partner_tip);

        // Feed the completed merge into the persistent participant cache
        // (sync.md §5.2) — the raw material for fraud claims (blockchain.md §11.9).
        if (!state.own_tip_cbor.empty()) {
            chainsync::ParticipantCache cache(data_dir / "sync_cache");
            const auto own_tip = Serializer::decode_tip(
                state.own_tip_cbor.data(), state.own_tip_cbor.size());
            const auto own_snap = Serializer::decode_snapshot(
                state.own_snap_cbor.data(), state.own_snap_cbor.size());
            const auto peer_snap = Serializer::decode_snapshot(
                state.peer_snap_cbor.data(), state.peer_snap_cbor.size());
            const Hash union_root = chainsync::record_merge(
                cache, own_tip, own_snap, partner_tip, peer_snap);
            std::cerr << "Participant cache updated: union root "
                      << to_hex(union_root.bytes) << "\n";
        }
    }

    fs::remove(merge_state_path(data_dir, leaf));

    const Hash merge_hash = Crypto::hash_block(finalized);
    std::cout << "merge block #" << finalized.address.block_index
              << "  hash: " << to_hex(merge_hash.bytes) << "\n";
    std::cerr << "Merge complete. Co-signature stored as seal.\n";
    std::cerr << "Seals: bc seal list " << to_hex(merge_hash.bytes) << "\n";
    return 0;
}

// ── Networked merge over the aggregator relay (sync.md §4.1) ──────────────────

static void print_merge_result(const Block& blk) {
    const Hash merge_hash = Crypto::hash_block(blk);
    const MergePayload mp = Serializer::decode_merge_payload(
        blk.payload.data(), blk.payload.size());
    std::cout << "merge block #" << blk.address.block_index
              << "  hash: " << to_hex(merge_hash.bytes) << "\n";
    std::cout << "union root: " << to_hex(mp.merkle_root.bytes) << "\n";
    std::cerr << "Merge complete. Participant cache updated.\n";
}

// Best-effort push of the local cache to the aggregator's snapshot warehouse
// (sync.md §7.1) — gossip must not fail a completed merge.
static void publish_cache_via(const std::string& via,
                              const chainsync::ParticipantCache& cache) {
    try {
        chainsync::HttpSnapshotStore store(via);
        const std::size_t n = chainsync::publish_cache(store, cache);
        if (n > 0)
            std::cerr << "Published " << n << " snapshot entr(ies) to " << via << "\n";
    } catch (const std::exception& e) {
        std::cerr << "snapshot publish skipped: " << e.what() << "\n";
    }
}

// bc merge run --peer UID_HEX --via URL [--leaf L] [--depth N] [--timeout SEC]
// Initiates one merge dialogue and drives it to completion over the relay.
static int cmd_merge_run(const fs::path& data_dir, int argc, char** argv) {
    const auto peer_hex = flag_val(argc, argv, "--peer");
    const auto via      = flag_val(argc, argv, "--via");
    if (peer_hex.empty() || via.empty()) {
        std::cerr << "Usage: bc merge run --peer UID_HEX --via URL "
                     "[--leaf L] [--depth N] [--timeout SEC]\n";
        return 1;
    }
    UserId peer{};
    if (!from_hex(peer_hex, peer.bytes.data(), 32)) {
        std::cerr << "Invalid --peer hex (expected 64 hex chars)\n";
        return 1;
    }

    const NodeIndex leaf    = parse_leaf_index(argc, argv);
    const long      timeout = std::stol(flag_val(argc, argv, "--timeout", "60"));
    const auto      dep_s   = flag_val(argc, argv, "--depth");
    const uint32_t  depth   = dep_s.empty() ? 1u
                            : static_cast<uint32_t>(std::stoul(dep_s));

    Context ctx(data_dir, leaf);

    // Freshness (blockchain.md §6.7 rule 11, sync.md §10.3): refresh the peer's revocations
    // from the same aggregator before opening a bilateral act.
    const auto rf = fetch_import_revocations(ctx.storage, peer, via);
    if (rf.imported > 0)
        std::cerr << "peer revocations refreshed: " << rf.imported
                  << " certificate(s)\n";

    MergeSession session(ctx.storage, ctx.validator);
    chainsync::ParticipantCache cache(data_dir / "sync_cache");
    const auto now = static_cast<Timestamp>(std::time(nullptr));
    chainsync::MergeDialogue dialogue(
        session, cache,
        chainsync::MergeConfig{ctx.user_id, leaf, ctx.working_kp, now, depth});
    chainsync::HttpDialogueChannel channel(via, ctx.user_id);
    chainsync::DialoguePump pump(dialogue, channel, ctx.user_id, peer,
                                 chainsync::make_session_id());

    if (!pump.begin()) {
        if (dialogue.failed())
            std::cerr << "merge failed: " << dialogue.error() << "\n";
        else
            std::cerr << "relay unreachable: " << via << "\n";
        return 1;
    }
    std::cerr << "OFFER sent to " << peer_hex.substr(0, 16) << "... via " << via
              << " (partner must be running: bc merge serve)\n";

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout);
    while (!pump.finished() && std::chrono::steady_clock::now() < deadline) {
        for (auto& env : channel.poll())
            if (!pump.feed(env)) { std::cerr << "relay send failed\n"; return 1; }
        if (!pump.finished())
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (dialogue.done()) {
        print_merge_result(*dialogue.merge_block());
        publish_cache_via(via, cache);
        upload_block(via, *dialogue.merge_block());   // feeds discovery (sync.md §8)
        return 0;
    }
    if (dialogue.failed()) { std::cerr << "merge failed: " << dialogue.error() << "\n"; return 1; }
    std::cerr << "no answer within " << timeout
              << "s — dialogue stalled and abandoned (safe to retry)\n";
    return 1;
}

// bc merge own --with NODE [--leaf L] [--depth N]
// Internal merge (blockchain.md §3.2/blockchain.md §5.3): the --leaf branch (public vertex)
// merges with own branch NODE in-process; both branches append co-signed
// MERGE blocks and the vertex snapshot commits both.
static int cmd_merge_own(const fs::path& data_dir, int argc, char** argv) {
    const auto with_s = flag_val(argc, argv, "--with");
    if (with_s.empty()) {
        std::cerr << "Usage: bc merge own --with NODE_INDEX [--leaf L] [--depth N]\n";
        return 1;
    }
    NodeIndex with = 0;
    try {
        const unsigned long long v = std::stoull(with_s, nullptr, 0);
        if (v > 0xFFFF'FFFEu || !is_valid_node(static_cast<NodeIndex>(v)))
            throw std::runtime_error("bad node");
        with = static_cast<NodeIndex>(v);
    } catch (...) {
        std::cerr << "Invalid --with node index: " << with_s << "\n";
        return 1;
    }
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    if (with == leaf) {
        std::cerr << "--with must name a different branch\n";
        return 1;
    }

    Context ctx(data_dir, leaf);
    const KeyPair with_kp = load_keypair(data_dir, with);   // branch init first
    const auto     dep_s  = flag_val(argc, argv, "--depth");
    const uint32_t depth  = dep_s.empty() ? 1u
                          : static_cast<uint32_t>(std::stoul(dep_s));
    const auto now = static_cast<Timestamp>(std::time(nullptr));

    MergeSession session_a(ctx.storage, ctx.validator);
    MergeSession session_b(ctx.storage, ctx.validator);
    chainsync::ParticipantCache cache(data_dir / "sync_cache");
    chainsync::MergeDialogue a(session_a, cache,
        chainsync::MergeConfig{ctx.user_id, leaf, ctx.working_kp, now, depth});
    chainsync::MergeDialogue b(session_b, cache,
        chainsync::MergeConfig{ctx.user_id, with, with_kp, now, depth});

    // In-memory pump: deliver messages until both dialogues go quiet.
    std::deque<std::vector<uint8_t>> to_b, to_a;
    for (auto& m : a.start()) to_b.push_back(std::move(m));
    while (!to_b.empty() || !to_a.empty()) {
        if (!to_b.empty()) {
            auto msg = std::move(to_b.front());
            to_b.pop_front();
            for (auto& r : b.on_message(msg.data(), msg.size()))
                to_a.push_back(std::move(r));
        } else {
            auto msg = std::move(to_a.front());
            to_a.pop_front();
            for (auto& r : a.on_message(msg.data(), msg.size()))
                to_b.push_back(std::move(r));
        }
    }

    if (!a.done() || !b.done()) {
        std::cerr << "internal merge failed: "
                  << (a.failed() ? a.error() : b.error()) << "\n";
        return 1;
    }
    std::cerr << "branch " << leaf << " <-> branch " << with << ":\n";
    print_merge_result(*a.merge_block());
    return 0;
}

// bc merge serve --via URL [--leaf L] [--depth N] [--timeout SEC] [--once]
// Responder loop: picks up OFFERs from the own mailbox and answers each with
// the given branch. --once exits after the first completed merge.
static int cmd_merge_serve(const fs::path& data_dir, int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc merge serve --via URL "
                     "[--leaf L] [--depth N] [--timeout SEC] [--once]\n";
        return 1;
    }

    const NodeIndex leaf    = parse_leaf_index(argc, argv);
    const bool      once    = flag_present(argc, argv, "--once");
    const long      timeout = std::stol(flag_val(argc, argv, "--timeout", "0"));
    const auto      dep_s   = flag_val(argc, argv, "--depth");
    const uint32_t  depth   = dep_s.empty() ? 1u
                            : static_cast<uint32_t>(std::stoul(dep_s));

    Context ctx(data_dir, leaf);
    chainsync::ParticipantCache cache(data_dir / "sync_cache");
    chainsync::HttpDialogueChannel channel(via, ctx.user_id);

    // One dialogue per (session, sender). Concurrent dialogues on the same
    // branch resolve themselves: the second create_pending fails its dialogue.
    struct Serving {
        MergeSession             session;
        chainsync::MergeDialogue dialogue;
        chainsync::DialoguePump  pump;

        Serving(Context& ctx, chainsync::ParticipantCache& cache, NodeIndex leaf,
                uint32_t depth, chainsync::IDialogueChannel& ch,
                const chainsync::RelayEnvelope& env)
            : session(ctx.storage, ctx.validator)
            , dialogue(session, cache,
                       chainsync::MergeConfig{
                           ctx.user_id, leaf, ctx.working_kp,
                           static_cast<Timestamp>(std::time(nullptr)), depth})
            , pump(dialogue, ch, ctx.user_id, env.from, env.session) {}
    };
    std::map<std::string, std::unique_ptr<Serving>> serving;

    std::cerr << "Serving merges via " << via
              << (once ? " (first merge, then exit)" : " (Ctrl+C to stop)") << "\n";

    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(timeout);
    for (;;) {
        for (const auto& env : channel.poll()) {
            const std::string key =
                to_hex(env.session.data(), env.session.size())
                + to_hex(env.from.bytes.data(), env.from.bytes.size());
            auto it = serving.find(key);
            if (it == serving.end()) {
                it = serving.emplace(key, std::make_unique<Serving>(
                         ctx, cache, leaf, depth, channel, env)).first;
                std::cerr << "Dialogue opened by "
                          << to_hex(env.from.bytes.data(), 8) << "...\n";
            }
            if (!it->second->pump.feed(env)) {
                std::cerr << "relay send failed — dropping dialogue\n";
                it->second = nullptr;
            }
        }

        for (auto it = serving.begin(); it != serving.end();) {
            if (!it->second) { it = serving.erase(it); continue; }
            auto& d = it->second->dialogue;
            if (d.done()) {
                print_merge_result(*d.merge_block());
                publish_cache_via(via, cache);
                upload_block(via, *d.merge_block());   // feeds discovery (sync.md §8)
                if (once) return 0;
                it = serving.erase(it);
            } else if (d.failed()) {
                std::cerr << "dialogue failed: " << d.error() << "\n";
                it = serving.erase(it);
            } else {
                ++it;
            }
        }

        if (timeout > 0 && std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "timeout after " << timeout << "s\n";
            return once ? 1 : 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
}

// ── JSON over HTTP: shared by bc trust, bc deal, bc match ─────────────────────

// GET a JSON endpoint and parse it. nullopt on any failure.
static std::optional<records::json::Value> fetch_json(const std::string& via,
                                                      const std::string& path) {
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get(path);
    if (!res || res->status != 200) return std::nullopt;
    try {
        return records::json::Reader(res->body).read();
    } catch (const records::json::JsonError&) {
        return std::nullopt;
    }
}

static std::string jstr(const records::json::Object& o, const char* k) {
    const auto* v = records::json::find(o, k);
    return (v && v->is_string()) ? v->string : std::string{};
}
static double jnum(const records::json::Object& o, const char* k) {
    const auto* v = records::json::find(o, k);
    return (v && v->is_number()) ? v->number : 0.0;
}
static const records::json::Value* jarr(const records::json::Object& o,
                                        const char* k) {
    const auto* v = records::json::find(o, k);
    return (v && v->is_array()) ? v : nullptr;
}

// ── Economy: named labor-hours (records.md §11, records.md §12.7) ────────────────────────

static std::array<uint8_t, 32> uid_from_hex(const std::string& s) {
    std::array<uint8_t, 32> a{};
    if (!from_hex(s, a.data(), 32))
        throw std::runtime_error("invalid UserId hex (expected 64 hex chars): " + s);
    return a;
}

// Analytic wallet view — no protocol enforcement (records.md §12.7).
// Per-branch purses (economy.md §5а): holdings belong to the selected branch —
// outgoing transfers written in it spend from it, incoming ones credited via
// to_node fill it. Debt stays chain-level: issue/redemption sum over ALL
// local branches — the person owes, whichever pocket spent.
struct WalletView {
    std::map<std::string, double> holdings;   // issuer hex → units held by this branch
    double issued   = 0;                      // own paper in circulation (chain-wide)
    double redeemed = 0;                      // own paper annihilated (chain-wide)
    double debt() const { return issued - redeemed; }
};

static WalletView compute_wallet(Context& ctx) {
    WalletView w;
    const auto& me = ctx.user_id.bytes;

    for (const NodeIndex branch : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, branch); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            try {
                const auto rec = Codec::decode(b.payload.data(), b.payload.size());
                const auto* t  = std::get_if<records::Transfer>(&rec);
                if (!t || t->from != me) continue;
                for (const auto& o : t->origins) {
                    if (o.issuer == me) w.issued += o.units;   // chain-level debt
                    else if (branch == ctx.leaf)               // this purse spent
                        w.holdings[to_hex(o.issuer.data(), 32)] -= o.units;
                }
            } catch (const CodecError&) {}
        }
    }

    ctx.storage.for_each_external_block([&](const Block& b) {
        if (b.type != BlockType::DATA) return true;
        try {
            const auto rec = Codec::decode(b.payload.data(), b.payload.size());
            const auto* t  = std::get_if<records::Transfer>(&rec);
            if (!t || t->to != me) return true;
            if (b.address.user_id.bytes != t->from) return true;      // spoofed sender
            for (const auto& o : t->origins) {
                if (o.issuer == me) w.redeemed += o.units;            // redemption
                else if (t->to_node == ctx.leaf)                      // this purse credited
                    w.holdings[to_hex(o.issuer.data(), 32)] += o.units;
            }
        } catch (const CodecError&) {}
        return true;
    });

    for (auto it = w.holdings.begin(); it != w.holdings.end();)
        it = std::abs(it->second) < 1e-9 ? w.holdings.erase(it) : std::next(it);
    return w;
}

// Auto-selection of portions (records.md §11.1): receiver's own paper first
// (it will annihilate), then other held paper, self-issue for the remainder.
static std::vector<records::OriginQty> pick_portions(Context& ctx,
                                                     const std::string& to_hex_s,
                                                     double units) {
    if (units <= 0) throw std::runtime_error("units must be positive");
    std::vector<records::OriginQty> portions;
    double remaining = units;
    auto wallet = compute_wallet(ctx);
    auto spend = [&](const std::string& issuer_hex) {
        auto it = wallet.holdings.find(issuer_hex);
        if (it == wallet.holdings.end() || it->second <= 0 || remaining <= 1e-9)
            return;
        const double take = std::min(it->second, remaining);
        portions.push_back({uid_from_hex(issuer_hex), take});
        remaining -= take;
        it->second = 0;
    };
    spend(to_hex_s);                                        // redemption first
    for (const auto& [issuer, held] : wallet.holdings) {
        (void)held;
        spend(issuer);
    }
    if (remaining > 1e-9)
        portions.push_back({ctx.user_id.bytes, remaining});  // self-issue
    return portions;
}

// ── Emission thread (economy.md §4.3) ─────────────────────────────────────────

struct EmissionThreadState {
    uint64_t                    next_seq = 0;
    double                      debt     = 0;  // declared debt at the thread tip
    std::optional<records::Ref> tip;           // last link block
};

// The thread tip across all local branches: self-issues (Transfer.emission)
// and redemption receipts share one chain-wide seq space.
static EmissionThreadState emission_thread_state(Context& ctx) {
    EmissionThreadState st{};
    bool found = false;

    for (const NodeIndex branch : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, branch); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            records::Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }

            const records::EmissionLink* link = nullptr;
            if (const auto* t = std::get_if<records::Transfer>(&rec)) {
                if (t->from == ctx.user_id.bytes && t->emission) link = &*t->emission;
            } else if (const auto* rd = std::get_if<records::Redemption>(&rec)) {
                link = &rd->link;
            }
            if (!link) continue;
            if (!found || link->seq + 1 > st.next_seq) {
                found       = true;
                st.next_seq = link->seq + 1;
                st.debt     = link->debt_after;
                st.tip      = records::Ref{ctx.user_id.bytes,
                                           Crypto::hash_block(b).bytes};
            }
        }
    }
    return st;
}

// Fill the transfer's emission-thread link when it self-issues (Transfer v3).
static void attach_emission_link(Context& ctx, records::Transfer& t) {
    double self_issued = 0;
    for (const auto& o : t.origins)
        if (o.issuer == t.from) self_issued += o.units;
    if (self_issued <= 1e-9) return;

    const auto st = emission_thread_state(ctx);
    records::EmissionLink link{};
    link.seq        = st.next_seq;
    link.prev       = st.tip;
    link.debt_after = st.debt - self_issued;
    t.emission = link;
    std::cerr << "  emission link #" << link.seq
              << "  declared debt " << link.debt_after << "h\n";
}

// Best-effort upload so the receiver can fetch the block from the aggregator.
static void upload_block(const std::string& via, const Block& block) {
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto cbor = Serializer::encode(block);
    const auto res  = cli.Post("/blocks",
        std::string(reinterpret_cast<const char*>(cbor.data()), cbor.size()),
        "application/cbor");
    if (res && res->status == 200)
        std::cerr << "Uploaded to " << via << "\n";
    else
        std::cerr << "upload to " << via << " failed — receiver can't fetch it yet\n";
}

static void print_portions(const records::Transfer& t, const std::string& me_hex,
                           const std::string& to_hex_s) {
    for (const auto& o : t.origins) {
        const std::string issuer = to_hex(o.issuer.data(), 32);
        const char* kind = issuer == me_hex   ? "self-issued"
                         : issuer == to_hex_s ? "returned to issuer"
                                              : "endorsed";
        std::cerr << "  " << o.units << "h  " << kind
                  << "  (issuer " << issuer.substr(0, 16) << "...)\n";
    }
}

// bc wallet [--leaf L]
static int cmd_wallet(const fs::path& data_dir, int argc, char** argv) {
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    const auto w = compute_wallet(ctx);

    // blockchain.md §6.7: surface the purse branch's own revocation state.
    if (auto rst = ctx.validator.effective_revocation(ctx.user_id, ctx.leaf))
        std::cout << (rst->replacement_pubkey.has_value()
                          ? "note: this branch was revoked and REPLACED (blockchain.md §6.7)\n"
                          : "WARNING: this branch is FROZEN by revocation (blockchain.md §6.7)\n");

    double held = 0;
    std::cout << "purse of branch 0x" << std::hex << ctx.leaf << std::dec
              << " (paper of others):\n";
    if (w.holdings.empty()) std::cout << "  (none)\n";
    for (const auto& [issuer, units] : w.holdings) {
        std::cout << "  " << issuer.substr(0, 16) << "...  " << units << "h\n";
        held += units;
    }
    std::cout << "total held: " << held << "h\n"
              << "own debt in circulation: " << w.debt()
              << "h  (issued " << w.issued << ", redeemed " << w.redeemed << ")\n";

    // Emission thread (economy.md §4.3): the declared, self-attested debt level.
    const auto th = emission_thread_state(ctx);
    if (th.tip.has_value()) {
        std::cout << "emission thread: link #" << (th.next_seq - 1)
                  << "  declared debt " << th.debt << "h";
        if (std::abs(-th.debt - w.debt()) > 1e-6)
            std::cout << "  (differs from computed " << -w.debt()
                      << "h — receipts pending?)";
        std::cout << "\n";
    } else {
        std::cout << "emission thread: empty (no self-issues yet)\n";
    }
    return 0;
}

// ── Credit history — bc trust (ИР-010 layer 1, records.md §11.6) ─────────────

// Collect the subject's emission-thread links from blocks available locally:
// own branches when the subject is this chain, external blocks otherwise.
// Layer 1 is deliberately local-only (решение 2026-07-16): what YOU hold
// after merges, verified by no one else — the honest personal viewpoint.
static std::vector<records::ObservedLink> observed_links(
        Context& ctx, const std::array<uint8_t, 32>& subject) {
    std::vector<records::ObservedLink> out;
    const auto digest = [&](const Block& b) {
        if (b.type != BlockType::DATA) return;
        records::Record rec;
        try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
        catch (const CodecError&) { return; }

        records::ObservedLink o{};
        if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->from != subject || !t->emission) return;  // spoofed / unthreaded
            o.link = *t->emission;
            for (const auto& p : t->origins)
                if (p.issuer == t->from) o.units += p.units;
            o.timestamp = t->timestamp;
        } else if (const auto* rd = std::get_if<records::Redemption>(&rec)) {
            o.link          = rd->link;
            o.units         = rd->units;
            o.is_redemption = true;
            o.timestamp     = rd->timestamp;
        } else {
            return;
        }
        o.block_hash = Crypto::hash_block(b).bytes;
        out.push_back(o);
    };

    if (subject == ctx.user_id.bytes) {
        for (const NodeIndex branch : ctx.local_branches()) {
            std::vector<Block> blocks;
            try { blocks = ctx.bc.get_branch(ctx.user_id, branch); }
            catch (const BlockchainError&) { continue; }
            for (const auto& b : blocks) digest(b);
        }
    } else {
        ctx.storage.for_each_external_block([&](const Block& b) {
            if (b.address.user_id.bytes == subject) digest(b);
            return true;
        });
    }
    return out;
}

// bc trust CHAIN_HEX_OR_PREFIX [--via URL] [--leaf L]
// Layers 2–3 of the footprint (ИР-010): computed by the aggregator over the
// whole graph, so every figure is marked "со слов агрегатора" and carries its
// own forging price. Reachability is judged HERE, from the asker's own view:
// a holder whose blocks you already hold is someone you can actually ask.
static void print_footprint(Context& ctx,
                            const std::array<uint8_t, 32>& subject,
                            const std::string& via) {
    if (via.empty()) {
        std::cout << "\n  (слои 2–3 — держатели бумаги и замкнутость — "
                     "считает агрегатор: добавьте --via URL)\n";
        return;
    }
    const auto doc = fetch_json(via, "/economy/footprint/" + to_hex(subject));
    if (!doc || !doc->is_object()) {
        std::cout << "\n  (агрегатор " << via << " не знает эту цепь либо "
                     "недоступен — слои 2–3 пропущены)\n";
        return;
    }
    const auto& o = doc->object;

    // Chains whose blocks are in the local store: you have met them, so you
    // can verify their side yourself instead of taking the aggregator's word.
    std::set<std::string> seen_locally;
    seen_locally.insert(to_hex(ctx.user_id.bytes));
    ctx.storage.for_each_external_block([&](const Block& b) {
        seen_locally.insert(to_hex(b.address.user_id.bytes));
        return true;
    });

    std::cout << "\nСЛОЙ 2 — КТО ВЗЯЛ ЕЁ РИСК   [со слов агрегатора " << via << ";\n"
                 "  перепроверяемо по сырым записям. Цена подделки: нужны "
                 "живые\n  контрагенты, согласившиеся держать бумагу и "
                 "принять труд]\n";

    const auto* holders = jarr(o, "holders");
    const std::size_t n_holders = holders ? holders->array.size() : 0;
    if (n_holders == 0) {
        std::cout << "  бумагу этой цепи сейчас не держит никто — ей ещё "
                     "никто не поверил\n";
    } else {
        std::size_t reachable = 0;
        for (const auto& hv : holders->array)
            if (hv.is_object() && seen_locally.count(jstr(hv.object, "chain")))
                ++reachable;
        std::cout << "  держат её бумагу: " << n_holders << " цеп. на "
                  << jnum(o, "paper_outstanding") << "h";
        std::cout << "\n  из них в вашей досягаемости (их блоки у вас есть): "
                  << reachable << "\n";
        for (const auto& hv : holders->array) {
            if (!hv.is_object()) continue;
            const auto chain = jstr(hv.object, "chain");
            std::cout << "    " << chain.substr(0, 16) << "…  "
                      << jnum(hv.object, "units") << "h"
                      << (seen_locally.count(chain) ? "  (знакома вам)" : "")
                      << "\n";
        }
    }

    const auto redeemers = jnum(o, "redeemers");
    if (redeemers > 0)
        std::cout << "  вернула трудом бумагу " << static_cast<int>(redeemers)
                  << " цеп. на " << jnum(o, "redeemed_to_others")
                  << "h — за это надо было работать\n";
    else
        std::cout << "  ни одна её бумага ещё не вернулась к ней трудом\n";

    const auto acceptors = jnum(o, "acceptors");
    if (acceptors > 0)
        std::cout << "  её труд приняли " << static_cast<int>(acceptors)
                  << " посторонних цеп. на " << jnum(o, "labor_outward")
                  << "h — они получили что-то настоящее\n";
    else
        std::cout << "  её труд не принимал никто посторонний\n";

    const auto* ht = records::json::find(o, "has_turnover");
    if (!ht || !ht->boolean) {
        std::cout << "\nСЛОЙ 3 — ЗАМКНУТОСТЬ: оборота нет, судить не о чем\n";
        return;
    }
    const double closed  = jnum(o, "closedness");
    const int    parties = static_cast<int>(jnum(o, "counterparties"));
    std::cout << "\nСЛОЙ 3 — ЗАМКНУТОСТЬ ОБОРОТА  "
              << static_cast<int>(std::round(100.0 * closed)) << "%"
              << "  (внутри круга " << jnum(o, "internal_turnover")
              << "h, наружу " << jnum(o, "boundary_turnover") << "h, "
              << parties << " контрагент.)\n"
                 "  [СКРИНИНГ, НЕ ДОКАЗАТЕЛЬСТВО: ловит только ленивую ферму —\n"
                 "   умная добавит жертвенные связи наружу (ИР-009)]\n";
    // Below a handful of counterparties there is no cluster to speak of: a
    // healthy pair trading honestly is 100% closed by arithmetic, and flagging
    // it would train people to ignore the flag.
    if (parties < 3)
        std::cout << "  контрагентов слишком мало, чтобы говорить о кластере —"
                     " замкнутость\n  здесь ничего не значит\n";
    else if (closed > 0.95)
        std::cout << "  ФЛАГ: ценность не покидает её круг — так выглядит "
                     "замкнутый кластер\n";
}

static int cmd_trust(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [trust, <chain>]
    if (pos.size() < 2) {
        std::cerr << "Usage: bc trust CHAIN_HEX_OR_PREFIX [--via URL] [--leaf L]\n";
        return 1;
    }
    Context ctx(data_dir, parse_leaf_index(argc, argv));

    // Resolve a prefix against chains seen locally (own + external blocks).
    const std::string& subj_s = pos[1];
    std::array<uint8_t, 32> subject{};
    if (subj_s.size() == 64) {
        subject = uid_from_hex(subj_s);
    } else {
        std::set<std::string> candidates;
        if (const auto me = to_hex(ctx.user_id.bytes); me.rfind(subj_s, 0) == 0)
            candidates.insert(me);
        ctx.storage.for_each_external_block([&](const Block& b) {
            if (const auto uid = to_hex(b.address.user_id.bytes);
                uid.rfind(subj_s, 0) == 0)
                candidates.insert(uid);
            return true;
        });
        if (candidates.empty()) {
            std::cerr << "цепь с префиксом '" << subj_s
                      << "' локально не встречалась\n";
            return 1;
        }
        if (candidates.size() > 1) {
            std::cerr << "префикс '" << subj_s << "' неоднозначен:\n";
            for (const auto& c : candidates)
                std::cerr << "  " << c.substr(0, 16) << "…\n";
            return 1;
        }
        subject = uid_from_hex(*candidates.begin());
    }

    const auto h = records::credit_history(observed_links(ctx, subject));

    std::cout << "КРЕДИТНАЯ ИСТОРИЯ  " << to_hex(subject).substr(0, 8) << "…\n"
              << "  [нить эмиссии в вашей локальной копии цепи; подделка =\n"
              << "   эквивокация нити — ловится при первом же расхождении]\n\n";

    if (h.links_seen == 0) {
        // An empty thread is not silence and not innocence: it means only
        // that YOUR copy has no links. Layers 2–3 below may still show a lot,
        // including a debt this chain never showed you — so state the two
        // possible causes and assert neither.
        std::cout << "  нить эмиссии в вашей картине пуста — либо вы не "
                     "мержились\n  с этой цепью и её звенья до вас не дошли, "
                     "либо она ничего\n  не самоэмитировала. Что именно — "
                     "отсюда не видно.\n";
        print_footprint(ctx, subject, flag_val(argc, argv, "--via"));
        return 0;
    }

    std::cout << "  видно звеньев: " << h.links_seen << " из " << h.links_expected;
    if (h.gaps) std::cout << "  — КАРТИНА НЕПОЛНАЯ, судите осторожно";
    std::cout << "\n  выпущено " << h.issued << "h, погашено " << h.redeemed << "h";
    if (h.issued > 1e-9)
        std::cout << " ("
                  << static_cast<int>(std::round(100.0 * h.redeemed / h.issued))
                  << "%)";
    std::cout << "\n  пик долга: " << h.max_debt
              << "h, максимально возвращено с пика: " << h.max_repaid
              << "h («столько доверили — вернул»)\n"
              << "  объявленный долг сейчас: " << h.outstanding << "h\n";

    const auto days_ago = [](int64_t ts) {
        return (static_cast<int64_t>(std::time(nullptr)) - ts) / 86400;
    };
    if (h.last_redemption_ts)
        std::cout << "  последнее погашение: "
                  << days_ago(*h.last_redemption_ts) << " дн. назад\n";
    if (h.last_selfissue_ts)
        std::cout << "  последняя самоэмиссия: "
                  << days_ago(*h.last_selfissue_ts) << " дн. назад\n";

    if (h.growth_without_redemption)
        std::cout << "\n  ФЛАГ: долг только растёт — ни одного погашения "
                     "(ИР-010, слой 1)\n";
    if (h.thread_inconsistent)
        std::cout << "\n  ФЛАГ: объявленный долг не сходится с суммой звеньев — "
                     "нить ведётся нечестно\n";
    for (const auto seq : h.equivocated_seqs)
        std::cout << "\n  ЭКВИВОКАЦИЯ: звено #" << seq
                  << " существует в двух разных блоках — объективное "
                     "доказательство обмана (economy.md §4.3)\n";

    print_footprint(ctx, subject, flag_val(argc, argv, "--via"));
    return 0;
}

// bc transfer recv <chain>/<hash> --via URL [--leaf L]
static int cmd_transfer_recv(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);
    const auto via = flag_val(argc, argv, "--via");
    if (pos.size() < 3 || via.empty()) {
        std::cerr << "Usage: bc transfer recv <chain_hex>/<hash_hex> --via URL [--leaf L]\n";
        return 1;
    }
    const Ref src = parse_ref(pos[2]);

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    // Freshness (blockchain.md §6.7 rule 11, sync.md §10.3): refresh the sender's revocations
    // before accepting value — the zones apply at acceptance time.
    UserId sender{};
    sender.bytes = src.chain;
    fetch_import_revocations(ctx.storage, sender, via);

    const Block block = fetch_block_from(via, src);
    if (block.type != BlockType::DATA)
        throw std::runtime_error("referenced block is not a DATA block");

    // Refuse paper from a revoked paying branch unless the transfer block runs
    // under the replacement key (blockchain.md §6.7: frozen/stolen purses cannot pay anew).
    if (auto rst = ctx.validator.effective_revocation(sender, block.address.node_index)) {
        const bool under_replacement = rst->replacement_pubkey.has_value()
            && block_signed_by(block, *rst->replacement_pubkey);
        if (!under_replacement)
            throw std::runtime_error(
                "sender's paying branch is revoked (blockchain.md §6.7) — transfer refused");
    }

    const auto rec = Codec::decode(block.payload.data(), block.payload.size());
    const auto* t  = std::get_if<records::Transfer>(&rec);
    if (!t) throw std::runtime_error("referenced record is not a Transfer");
    if (t->to != ctx.user_id.bytes)
        throw std::runtime_error("transfer is not addressed to this identity");
    if (t->from != block.address.user_id.bytes)
        throw std::runtime_error("transfer 'from' does not match the authoring chain");
    // Strict equivalence (records.md §12.9): hours move to another chain only as
    // a deal — the acceptance inside its payment (v5). The codec has already
    // refused anything else; this names it for the person.
    if (!t->accept)
        throw std::runtime_error(
            "перевод без приёмки — не оплата труда, такой не принимается (records.md §12.9)");
    // Кому достаётся результат, решает продавец — то есть мы (records.md §9.5):
    // сделку за работу, которую мы этому плательщику не отпускали, не принимаем —
    // она ничтожна, и её часы в кошелёк не идут.
    {
        std::optional<std::array<uint8_t, 32>> customer;
        if (const auto w = load_record(ctx, "", t->accept->work))
            if (const auto* wr = std::get_if<WorkRecord>(&*w)) customer = wr->customer;
        std::optional<records::Release>  release;
        std::optional<records::Transfer> bought;   // own purchase, bought back on its return
        if (t->accept->release)
            if (const auto r = load_record(ctx, "", *t->accept->release)) {
                if (const auto* rl = std::get_if<records::Release>(&*r)) release = *rl;
                else if (const auto* p = std::get_if<records::Transfer>(&*r)) bought = *p;
            }
        // Выкуп по заранее подписанному возврату и выкуп с адресом (records.md §9.8
        // v5, v6): цена возврата — уплаченное нами, для аренды — за вычетом износа.
        if (bought) {
            const auto h = load_holding(ctx, via, *t->accept->release);
            if (!h || !records::completes_buyback(
                          *t, *bought,
                          records::buyback_price(h->lots, h->work, bought->timestamp,
                                                 t->timestamp, bought->accept->buyback->wear)))
                throw std::runtime_error("выкуп не по подписанному возврату или не по его цене "
                                         "(или записей не прочесть — нужен --via): сделка ничего "
                                         "не завершает, часы не принимаются (records.md §9.8)");
        }
        if (release && release->addressed &&
            !records::honours_buyout(*t->accept, *release->addressed))
            throw std::runtime_error("покупка по выкупу с адресом без подписанного возврата "
                                     "— она ничего не завершает (records.md §9.8)");
        if (!bought && !records::addressed_to(*t, customer, release ? &*release : nullptr))
            throw std::runtime_error(
                "сделка не по адресу: вы не отпускали эту работу плательщику — ни "
                "заказчиком в записи труда, ни отпуском (или отпуск истёк). Такая "
                "сделка ничтожна, часы не принимаются (records.md §9.5)");
        if (release && release->holding) {
            // Перепродажа своей покупки (records.md §9.8 v4) — по цене покупки.
            const auto h = load_holding(ctx, via, *release->holding);
            const records::HoldingLot* hl = nullptr;
            if (h)
                for (const auto& l : h->lots)
                    if (l.out == release->lot->out) hl = &l;
            if (!hl || !records::meets_resale(*t->accept,
                                              records::resale_price(*hl, release->lot->qty)))
                throw std::runtime_error("перепродажа не по цене покупки — сделка ничего не "
                                         "завершает, часы не принимаются (records.md §9.8)");
        } else if (release && release->lot) {
            // Порция со склада — по ценнику (records.md §9.8 v3): иначе сделка ничего
            // не завершает, и её часы не наши.
            std::optional<records::PortionTerms> terms;
            if (const auto w = load_record(ctx, "", t->accept->work))
                if (const auto* wr = std::get_if<WorkRecord>(&*w))
                    terms = records::portion_terms(*wr, *release->lot);
            if (!terms || !records::meets_terms(*t->accept, *terms))
                throw std::runtime_error("сделка по порции не по ценнику партии — она ничего не "
                                         "завершает, часы не принимаются (records.md §9.8)");
        } else if (!bought && t->accept->axes.empty()) {
            // Приёмки без разбивки не существует (ИР-022): одним переносом платят
            // только за перепродажу или выкуп, а это — оплата нашей работы.
            throw std::runtime_error("оплата работы без разбивки по осям — такой приёмки нет, "
                                     "часы не принимаются (ИР-022)");
        }
    }
    // Transfer v3: a self-issuing transfer must be threaded (economy.md §4.3).
    {
        double self_issued = 0;
        for (const auto& o : t->origins)
            if (o.issuer == t->from) self_issued += o.units;
        if (self_issued > 1e-9 && !t->emission)
            throw std::runtime_error(
                "self-issuing transfer has no emission-thread link — refused "
                "(economy.md §4.3)");
    }

    if (ctx.storage.has_external_block(block.address)) {
        std::cerr << "already received — wallet unchanged\n";
        return 0;
    }
    ctx.storage.put_external_block(block);
    append_record(ctx, Record{Copy{src}});   // on-chain acknowledgment (двусторонность)

    // Own paper returned → the "+" link of the emission thread (economy.md §4.3): write
    // the redemption receipt; the debt shrinks and the whole credit history
    // stays readable off the thread.
    double returned = 0;
    for (const auto& o : t->origins)
        if (o.issuer == ctx.user_id.bytes) returned += o.units;
    if (returned > 1e-9) {
        const auto th = emission_thread_state(ctx);
        records::Redemption rd{};
        rd.transfer        = src;
        rd.units           = returned;
        rd.link.seq        = th.next_seq;
        rd.link.prev       = th.tip;
        rd.link.debt_after = th.debt + returned;
        rd.timestamp       = static_cast<int64_t>(std::time(nullptr));
        append_record(ctx, Record{rd});
        std::cout << "redemption receipt #" << rd.link.seq << ": +" << returned
                  << "h  declared debt " << rd.link.debt_after << "h\n";
    }

    double total = 0;
    for (const auto& o : t->origins) total += o.units;
    std::cout << "received " << total << "h in " << t->origins.size()
              << " portion(s) from " << to_hex(t->from.data(), 32).substr(0, 16)
              << "...  (credited to branch 0x" << std::hex << t->to_node
              << std::dec << ")\n";
    const auto w = compute_wallet(ctx);
    std::cerr << "wallet: " << w.holdings.size() << " issuer(s) held, own debt "
              << w.debt() << "h\n";
    return 0;
}

// bc fetch <chain>/<hash> --via URL [--leaf L]
// Generic read access to another chain: fetches any block from an aggregator
// into the local external store (work records, ideas, pledges, ...).
static int cmd_fetch(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);
    const auto via = flag_val(argc, argv, "--via");
    if (pos.size() < 2 || via.empty()) {
        std::cerr << "Usage: bc fetch <chain_hex>/<hash_hex> --via URL [--leaf L]\n";
        return 1;
    }
    const Ref src = parse_ref(pos[1]);

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    const Block block = fetch_block_from(via, src);
    if (ctx.storage.has_external_block(block.address)) {
        std::cerr << "already known\n";
    } else {
        ctx.storage.put_external_block(block);
        std::cerr << "stored\n";
    }

    if (block.type == BlockType::DATA) {
        try {
            const auto rec = Codec::decode(block.payload.data(), block.payload.size());
            std::cout << record_summary(rec) << "\n";
        } catch (const CodecError&) {
            std::cout << "[unknown payload]\n";
        }
    }
    return 0;
}

// Shared GET for the aggregator economy view (records.md §13). Prints the
// JSON body as-is — every figure is re-checkable against the chains.
static int economy_get(const std::string& via, const std::string& path) {
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get(path);
    if (!res) {
        std::cerr << "aggregator unreachable: " << via << "\n";
        return 1;
    }
    std::cout << res->body << "\n";
    return res->status == 200 ? 0 : 1;
}

// bc rates --via URL
static int cmd_rates(int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc rates --via URL\n";
        return 1;
    }
    return economy_get(via, "/economy/rates");
}

// bc cloud --via URL [--coords] — the work cloud (ИР-022 шаг 11): samples of
// work others repeated with `bc accept --like`, placed by what their families
// were paid for (specialty-axes.md §10).
static int cmd_cloud(int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc cloud --via URL [--coords]\n";
        return 1;
    }
    return economy_get(via, flag_present(argc, argv, "--coords")
                                ? "/economy/cloud?coords=1" : "/economy/cloud");
}

// bc axis-define --slug danger --ru "Опасность" --description "..."
//                [--parent REF] [--same-as REF] [--via URL]
//
// Завести ось в своей цепи (ИР-022). Словарь труда решают люди: заводить ось
// волен каждый, сила у неё — от употребления, а не от чьего-то благословения.
// Отличает хорошую ось от плохой ТОЛЬКО аргумент: что она значит, почему это
// одна ось, а не две, и чем она не является. Ось без описания видна в книге осей
// как таковая — это и есть всё принуждение.
static int cmd_axis_define(const fs::path& data_dir, int argc, char** argv) {
    const auto slug = flag_val(argc, argv, "--slug");
    const auto ru   = flag_val(argc, argv, "--ru");
    const auto desc = flag_val(argc, argv, "--description");
    if (slug.empty() || ru.empty()) {
        std::cerr << "Usage: bc axis-define --slug ОСЬ --ru \"Название\"\n"
                     "    --description TEXT           аргумент: что значит, почему "
                     "это ОДНА ось,\n"
                     "                                 чем не является. Без него ось "
                     "считается\n"
                     "                                 незаявленной и видна такой в "
                     "книге осей\n"
                     "    [--parent CHAIN/HASH]        более широкая ось\n"
                     "    [--same-as CHAIN/HASH]       заявить, что это та же ось, "
                     "что и та\n"
                     "    [--via URL]                  опубликовать агрегатору\n";
        return 1;
    }
    if (desc.empty())
        std::cerr << "предупреждение: ось без --description войдёт в книгу осей как "
                     "неописанная\n";
    AxisDef a{};
    a.slug        = slug;
    a.ru          = ru;
    a.description = desc;
    a.timestamp   = static_cast<int64_t>(std::time(nullptr));
    if (const auto p = flag_val(argc, argv, "--parent");  !p.empty()) a.parent  = parse_ref(p);
    if (const auto q = flag_val(argc, argv, "--same-as"); !q.empty()) a.same_as = parse_ref(q);
    // Ссылка на определение И ЕСТЬ личность оси — её и подхватывают скрипты.
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    const Block block = append_record(ctx, Record{a});
    // Полный путь: по нему можно дойти до определения руками, хеш — контроль.
    std::cout << "axis ref: " << to_hex(block.address.user_id.bytes) << "/"
              << block.address.node_index << "/" << block.address.block_index << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    if (const auto via = flag_val(argc, argv, "--via"); !via.empty())
        upload_block(via, block);
    return 0;
}

// bc axis-ledger --via URL — куда труд сети ушёл на самом деле (ИР-022)
static int cmd_axis_ledger(int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc axis-ledger --via URL [--rent]\n"
                     "    --rent   карта ренты: что корзина заплатила сверх сетевой\n"
                     "             цены названных осей, как долго и КТО. Невязка —\n"
                     "             диагностика, не доплата: цена одна, и она уплачена\n";
        return 1;
    }
    std::string path = "/economy/axis-ledger";
    if (flag_present(argc, argv, "--rent")) path += "?rent=1";
    return economy_get(via, path);
}

// bc ideas top --via URL
static int cmd_ideas_top(int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc ideas top --via URL\n";
        return 1;
    }
    return economy_get(via, "/economy/ideas");
}

// bc chain info UID_HEX --via URL
static int cmd_chain_info(int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);
    const auto via = flag_val(argc, argv, "--via");
    if (pos.size() < 3 || via.empty()) {
        std::cerr << "Usage: bc chain info UID_HEX --via URL\n";
        return 1;
    }
    return economy_get(via, "/economy/chain/" + pos[2]);
}

// bc catalog [--via URL] [--search TEXT]
// The picker: what slugs exist, so nobody has to invent one.
static int cmd_catalog(int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc catalog --via URL [--search TEXT]\n";
        return 1;
    }
    const auto catalogs = fetch_catalogs(via);
    if (!catalogs) {
        std::cerr << "агрегатор не отдал каталог (запущен ли он с --catalog PATH?)\n";
        return 1;
    }
    const auto query = flag_val(argc, argv, "--search");
    const auto hits  = records::search(*catalogs, query);
    if (hits.empty()) {
        std::cout << "ничего не найдено по \"" << query << "\"\n";
        return 0;
    }
    std::string group;
    for (const auto* e : hits) {
        if (e->group != group) {
            group = e->group;
            std::cout << "\n" << group << "\n";
        }
        std::cout << "  " << std::left << std::setw(26) << e->slug << e->ru;
        if (!e->closed_by.empty()) {
            std::cout << "   ← закрывают:";
            for (const auto& p : e->closed_by) std::cout << " " << p;
        }
        std::cout << "\n";
    }
    return 0;
}

// bc directory --skill SLUG --via URL   /   bc needs list [--need SLUG] --via URL
// Who offers a skill, who needs what — straight off the aggregator's ProfileView.
static int directory_get(const std::string& via, const std::string& facet,
                         const std::string& slug) {
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get("/directory/" + facet + "/" + slug);
    if (!res) {
        std::cerr << "aggregator unreachable: " << via << "\n";
        return 1;
    }
    std::cout << res->body << "\n";
    return res->status == 200 ? 0 : 1;
}

static int cmd_directory(int argc, char** argv) {
    const auto via   = flag_val(argc, argv, "--via");
    const auto skill = flag_val(argc, argv, "--skill");
    if (via.empty() || skill.empty()) {
        std::cerr << "Usage: bc directory --skill SLUG --via URL\n";
        return 1;
    }
    return directory_get(via, "skills", skill);
}

static int cmd_needs_list(int argc, char** argv) {
    const auto via  = flag_val(argc, argv, "--via");
    const auto need = flag_val(argc, argv, "--need");
    if (via.empty()) {
        std::cerr << "Usage: bc needs list [--need SLUG] --via URL\n";
        return 1;
    }
    if (need.empty()) return economy_get(via, "/profiles");   // all open needs live here
    return directory_get(via, "needs", need);
}

// bc match --via URL [--json]
//
// The report the whole thing exists for: whose needs are closed by whose skills,
// what nobody can close, and who could grow into the gap. Rendered for a person;
// --json hands the raw report to an external AI (no LLM lives in here).
static int cmd_match(int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc match --via URL [--json]\n";
        return 1;
    }
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(10);
    const auto res = cli.Get("/match");
    if (!res) {
        std::cerr << "aggregator unreachable: " << via << "\n";
        return 1;
    }
    if (res->status != 200) {
        std::cerr << res->body << "\n";
        return 1;
    }
    if (flag_present(argc, argv, "--json")) {
        std::cout << res->body << "\n";
        return 0;
    }

    using records::json::Value;
    Value root;
    try {
        root = records::json::Reader(res->body).read();
    } catch (const records::json::JsonError& e) {
        std::cerr << "агрегатор вернул некорректный JSON: " << e.what() << "\n";
        return 1;
    }
    auto str = [](const records::json::Object& o, const char* k) -> std::string {
        const Value* v = records::json::find(o, k);
        return (v && v->is_string()) ? v->string : std::string{};
    };
    auto arr = [](const Value& v, const char* k) -> const Value* {
        const Value* f = v.is_object() ? records::json::find(v.object, k) : nullptr;
        return (f && f->is_array()) ? f : nullptr;
    };

    // ── Matches ───────────────────────────────────────────────────────────────
    std::size_t met = 0, unmet = 0;
    std::cout << "СТЫКОВКИ\n";
    if (const Value* needs = arr(root, "needs")) {
        for (const auto& n : needs->array) {
            if (!n.is_object()) continue;
            const auto  seeker = str(n.object, "seeker");
            const auto  text   = str(n.object, "text");
            const auto  urg    = str(n.object, "urgency");
            const Value* cands = arr(n, "candidates");
            const bool  has    = cands && !cands->array.empty();
            has ? ++met : ++unmet;

            std::cout << (has ? "  + " : "  - ") << short_hex_str(seeker)
                      << "  \"" << text << "\""
                      << (urg == "high" ? "  [срочно]" : "") << "\n";
            if (!has) {
                std::cout << "      никто не закрывает\n";
                continue;
            }
            for (const auto& c : cands->array) {
                if (!c.is_object()) continue;
                const Value* d = records::json::find(c.object, "distance_km");
                std::cout << "      закроет " << short_hex_str(str(c.object, "chain"))
                          << "  " << str(c.object, "slug");
                // Accepted work next to the claim (records.md §14.4): what others
                // accepted, axis by axis — the reader decides, nothing is scored.
                const auto   grade = str(c.object, "grade");
                const Value* ac    = records::json::find(c.object, "attested_chains");
                const Value* ah    = records::json::find(c.object, "attested_hours");
                const Value* ax    = records::json::find(c.object, "attested_axes");
                const int    att_chains = (ac && ac->is_number())
                                              ? static_cast<int>(ac->number) : 0;
                const double att_hours  = (ah && ah->is_number()) ? ah->number : 0;
                if (att_chains > 0) {
                    std::cout << ", принято: " << att_chains << " цеп., "
                              << att_hours << " ч";
                    if (ax && ax->is_array() && !ax->array.empty()) {
                        std::cout << " (";
                        bool first_axis = true;
                        for (const auto& e : ax->array) {
                            if (!e.is_object()) continue;
                            const auto   label = str(e.object, "label");
                            const Value* h     = records::json::find(e.object, "hours");
                            std::cout << (first_axis ? "" : ", ")
                                      << (label.empty() ? "ось без названия" : label);
                            if (h && h->is_number()) std::cout << " " << h->number << " ч";
                            first_axis = false;
                        }
                        std::cout << ")";
                    }
                } else {
                    std::cout << ", принятой работы нет";
                }
                if (!grade.empty()) std::cout << ", заявлен разряд " << grade;
                if (d && d->is_number() && d->number >= 0) {
                    std::ostringstream km;
                    km << std::fixed << std::setprecision(1) << d->number;
                    std::cout << ", " << km.str() << " км";
                } else {
                    std::cout << ", удалённо";
                }
                std::cout << "\n";
            }
        }
    }
    std::cout << "\n  закрыто " << met << " из " << (met + unmet) << "\n";

    // ── Rings ─────────────────────────────────────────────────────────────────
    if (const Value* rings = arr(root, "rings"); rings && !rings->array.empty()) {
        std::cout << "\nКОЛЬЦА ОБМЕНА (замыкаются без долга наружу)\n";
        for (const auto& ring : rings->array) {
            if (!ring.is_array()) continue;
            std::cout << "  ";
            for (std::size_t i = 0; i < ring.array.size(); ++i) {
                if (i) std::cout << " → ";
                std::cout << short_hex_str(ring.array[i].string);
            }
            if (!ring.array.empty())
                std::cout << " → " << short_hex_str(ring.array[0].string);
            std::cout << "\n";
        }
    }

    // ── Deficits ──────────────────────────────────────────────────────────────
    if (const Value* defs = arr(root, "deficits"); defs && !defs->array.empty()) {
        std::cout << "\nДЕФИЦИТ И ПЕРЕОБУЧЕНИЕ\n";
        for (const auto& d : defs->array) {
            if (!d.is_object()) continue;
            std::cout << "  " << str(d.object, "need")
                      << "  \"" << str(d.object, "text") << "\"\n";

            if (const Value* p = arr(d, "professions"); p && !p->array.empty()) {
                std::cout << "      закрыла бы профессия:";
                for (const auto& x : p->array) std::cout << " " << x.string;
                std::cout << "\n";
            } else {
                std::cout << "      профессией не закрывается\n";
            }
            if (const Value* a = arr(d, "aspiring"); a && !a->array.empty()) {
                std::cout << "      уже хотят это освоить:";
                for (const auto& x : a->array) std::cout << " " << short_hex_str(x.string);
                std::cout << "\n";
            } else if (const Value* w = arr(d, "willing"); w && !w->array.empty()) {
                std::cout << "      готовы переучиваться вообще:";
                for (const auto& x : w->array) std::cout << " " << short_hex_str(x.string);
                std::cout << "\n";
            } else {
                std::cout << "      переучиваться пока никто не готов\n";
            }
        }
    }
    return 0;
}

// ── bc deal — сделка как потребность с хвостом ссылок (records.md §8.6) ───────
//
// The verbs act on DealView state fetched from the aggregator, resolving every
// ref themselves: in the step-1 live run one deal took 12 commands and 7 manual
// copies of 64-hex hashes — a human will not do that twice. The vocabulary is
// ordinary ConceptLinks («берусь», «исполняет», «закрыто») — no new types.

// The deal for a need ref, out of GET /deals. nullopt when the aggregator
// does not know it (not uploaded yet, or no such need).
static std::optional<records::json::Value> find_deal(const std::string& via,
                                                     const std::string& need_ref) {
    const auto root = fetch_json(via, "/deals");
    if (!root || !root->is_object()) return std::nullopt;
    const auto* deals = jarr(root->object, "deals");
    if (!deals) return std::nullopt;
    for (const auto& d : deals->array)
        if (d.is_object() && jstr(d.object, "need_ref") == need_ref) return d;
    return std::nullopt;
}

// Own facts of one record shape, scanned across local branches — so the verbs
// can resolve "my skill" without the person copying hashes.
struct OwnFact { std::string ref; std::string label; };

template <typename Pick>
static std::vector<OwnFact> scan_own_facts(Context& ctx, Pick pick) {
    std::vector<OwnFact> out;
    for (const NodeIndex node : ctx.local_branches()) {
        std::vector<Block> blocks;
        try { blocks = ctx.bc.get_branch(ctx.user_id, node); }
        catch (const BlockchainError&) { continue; }
        for (const auto& b : blocks) {
            if (b.type != BlockType::DATA) continue;
            Record rec;
            try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
            catch (const CodecError&) { continue; }
            if (auto label = pick(rec))
                out.push_back(OwnFact{
                    to_hex(ctx.user_id.bytes) + "/" +
                        to_hex(Crypto::hash_block(b).bytes),
                    *label});
        }
    }
    return out;
}

// bc deal list --via URL [--all]
// My deals with their stage and — the point — the exact next command, refs
// filled in.
static int cmd_deal_list(const fs::path& data_dir, int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc deal list --via URL [--all]\n";
        return 1;
    }
    const auto root = fetch_json(via, "/deals");
    if (!root || !root->is_object()) {
        std::cerr << "aggregator unreachable or /deals failed: " << via << "\n";
        return 1;
    }
    const auto* deals = jarr(root->object, "deals");
    if (!deals) { std::cerr << "bad /deals response\n"; return 1; }

    const bool  all = flag_present(argc, argv, "--all");
    std::string me;
    try { me = to_hex(load_user_id(data_dir).bytes); }
    catch (const std::exception&) { /* no identity yet — show all */ }

    std::size_t shown = 0;
    for (const auto& d : deals->array) {
        if (!d.is_object()) continue;
        const auto seeker   = jstr(d.object, "seeker");
        const auto need_ref = jstr(d.object, "need_ref");
        const auto stage    = jstr(d.object, "stage");

        // my roles in this deal
        bool i_seek = !me.empty() && seeker == me;
        bool i_work = false, i_took = false;
        std::string first_taker, work_ref, acc_ref;
        double appraised = jnum(d.object, "appraised");
        double paid      = jnum(d.object, "paid");
        if (const auto* ts = jarr(d.object, "takers"))
            for (const auto& t : ts->array) {
                if (!t.is_object()) continue;
                if (first_taker.empty()) first_taker = jstr(t.object, "chain");
                if (jstr(t.object, "chain") == me) i_took = true;
            }
        if (const auto* ws = jarr(d.object, "works"))
            for (const auto& w : ws->array) {
                if (!w.is_object()) continue;
                if (jstr(w.object, "worker") == me) i_work = true;
                if (work_ref.empty()) work_ref = jstr(w.object, "ref");
                if (acc_ref.empty())  acc_ref  = jstr(w.object, "acceptance_ref");
            }
        if (!all && !(i_seek || i_work || i_took)) continue;
        ++shown;

        const auto urgency = jstr(d.object, "urgency");
        std::cout << "  «" << jstr(d.object, "text") << "»"
                  << (urgency == "high" ? "  [срочно]" : "") << "\n"
                  << "    заказчик " << short_hex_str(seeker)
                  << (i_seek ? " (вы)" : "") << "   стадия: " << stage;
        if (appraised > 0)
            std::cout << "   оплачено " << paid << "/" << appraised << "ч";
        std::cout << "\n";

        // the exact next move, refs filled in
        std::string hint;
        if (stage == "открыта" && !i_seek)
            hint = "bc deal take " + need_ref + " --via " + via;
        else if (stage == "есть желающие" && i_seek)
            hint = "bc deal hire " + need_ref + " --executor " + first_taker +
                   " --units N --via " + via;
        else if (stage == "нанят" && !i_seek)
            hint = "bc deal work " + need_ref +
                   " --hours H --action \"...\" --via " + via;
        else if (stage == "работа сделана" && i_seek && !work_ref.empty()) {
            hint = "bc fetch " + work_ref + " --via " + via +
                   "  &&  bc accept --work " + work_ref + " --via " + via;
            // своё активное обещание по сделке закрывается той же записью (v4)
            if (const auto* ps = jarr(d.object, "pledges"))
                for (const auto& p : ps->array) {
                    if (!p.is_object()) continue;
                    const auto* revoked = records::json::find(p.object, "revoked");
                    if (revoked && revoked->boolean) continue;
                    if (jstr(p.object, "pledger") == me &&
                        jnum(p.object, "settled") + 1e-9 < jnum(p.object, "units")) {
                        hint += " --pledge " + jstr(p.object, "ref");
                        break;
                    }
                }
        } else if (stage == "оплачено" && i_seek)
            hint = "bc deal settle " + need_ref + " --via " + via;
        if (!hint.empty()) std::cout << "    → ваш ход: " << hint << "\n";
        std::cout << "\n";
    }
    if (shown == 0)
        std::cout << (all ? "сделок нет\n"
                          : "ваших сделок нет (все — bc deal list --all)\n");
    return 0;
}

// bc deal take NEED_REF [--skill REF] [--leaf L] [--via URL]
// «Берусь»: my skill → your need (заявка, records.md §14.7).
static int cmd_deal_take(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);   // [deal, take, NEED_REF]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc deal take NEED_REF [--skill SKILL_REF] "
                     "[--leaf L] [--via URL]\n";
        return 1;
    }
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    std::string skill_ref = flag_val(argc, argv, "--skill");
    if (skill_ref.empty()) {
        const auto skills = scan_own_facts(ctx, [](const Record& rec)
                -> std::optional<std::string> {
            const auto* c = std::get_if<Concept>(&rec);
            if (!c) return std::nullopt;
            for (const auto& tag : c->tags)
                if (tag == "kind:skill") return c->text;
            return std::nullopt;
        });
        if (skills.size() == 1) {
            skill_ref = skills[0].ref;
        } else if (skills.empty()) {
            std::cerr << "у вас нет записи навыка (kind:skill) — сначала "
                         "заявите его: bc concept add ... --tag kind:skill\n";
            return 1;
        } else {
            std::cerr << "у вас несколько навыков — укажите --skill REF:\n";
            for (const auto& s : skills)
                std::cerr << "  " << s.ref << "  \"" << s.label << "\"\n";
            return 1;
        }
    }

    ConceptLink cl;
    cl.from = parse_ref(skill_ref);
    cl.to   = parse_ref(pos[2]);
    cl.kind = "берусь";
    if (cl.from.chain != ctx.user_id.bytes)
        throw std::runtime_error("--skill must reference your own record — "
                                 "you volunteer with YOUR skill (records.md §8.6)");
    const Block block = append_record(ctx, Record{cl});
    std::cout << "взялись: " << skill_ref << " → " << pos[2] << "\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc deal hire NEED_REF --executor UID --units N [--expires TS]
// Sugar for `pledge add --target NEED_REF` — hiring is a pledge with a name.
static int cmd_deal_hire(const fs::path& data_dir, int argc, char** argv) {
    const auto pos     = get_positionals(argc, argv);
    const auto exec_s  = flag_val(argc, argv, "--executor");
    const auto units_s = flag_val(argc, argv, "--units");
    if (pos.size() < 3 || exec_s.empty() || units_s.empty()) {
        std::cerr << "Usage: bc deal hire NEED_REF --executor UID_HEX --units N "
                     "[--expires TS] [--leaf L] [--via URL]\n";
        return 1;
    }
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    Pledge p{};
    p.target    = parse_ref(pos[2]);
    p.units     = std::stod(units_s);
    p.executor  = uid_from_hex(exec_s);
    p.timestamp = static_cast<int64_t>(std::time(nullptr));
    if (p.units <= 0) throw std::runtime_error("--units must be positive");
    if (p.target.chain != ctx.user_id.bytes)
        throw std::runtime_error("NEED_REF must be your own need — the "
                                 "customer hires (records.md §8.6)");
    const auto exp_s = flag_val(argc, argv, "--expires");
    if (!exp_s.empty()) p.expires = static_cast<int64_t>(std::stoll(exp_s));

    const Block block = append_record(ctx, Record{p});
    std::cout << "наняли " << short_hex_str(exec_s) << " на " << p.units
              << "ч  (обещание " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(Crypto::hash_block(block).bytes) << ")\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc deal work NEED_REF --hours H --action TEXT [--title TEXT]
// Logs the work AND attaches it to the need («исполняет») in one go — the
// link is exactly what was lost in direct use of `bc work log` (ИР-006 разрыв 3).
static int cmd_deal_work(const fs::path& data_dir, int argc, char** argv) {
    const auto pos      = get_positionals(argc, argv);
    const auto hours_s  = flag_val(argc, argv, "--hours");
    const auto action_s = flag_val(argc, argv, "--action");
    if (pos.size() < 3 || hours_s.empty() || action_s.empty()) {
        std::cerr << "Usage: bc deal work NEED_REF --hours H --action TEXT "
                     "[--title TEXT] [--for CHAIN] [--axis-share AXIS=SHARE]... "
                     "[--tool HASH_PREFIX:USED]... "
                     "[--start TS] [--leaf L] [--via URL]\n"
                     "  работа — на заказ хозяина потребности (records.md §9.4);\n"
                     "  --for — если принимает и платит другой (economy.md §7)\n";
        return 1;
    }
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    WorkRecord wr{};
    wr.title    = flag_val(argc, argv, "--title");
    // Работа по потребности — на заказ её хозяина (records.md §9.4 v3): результат
    // отпущен ему этой же записью. Финансирует другой — --for: каждый
    // финансирующий принимает свою работу (economy.md §7).
    const auto for_s = flag_val(argc, argv, "--for");
    wr.customer = for_s.empty() ? parse_ref(pos[2]).chain : uid_from_hex(for_s);
    wr.action   = action_s;
    wr.hours    = std::stod(hours_s);
    // Без --start работа считается только что законченной (records.md §9.4 v4).
    const auto start_s = flag_val(argc, argv, "--start");
    wr.start_ts = start_s.empty()
        ? static_cast<int64_t>(std::time(nullptr)) - std::llround(wr.hours * 3600.0)
        : static_cast<int64_t>(std::stoll(start_s));
    wr.axes = parse_axis_shares(flag_all(argc, argv, "--axis-share"));
    auto carry_specs = flag_all(argc, argv, "--tool");
    for (const auto& s : flag_all(argc, argv, "--material")) carry_specs.push_back(s);
    if (!carry_specs.empty()) attach_carry(ctx, wr, carry_specs);
    place_in_thread(ctx, wr, !start_s.empty());

    const Block work_block = append_record(ctx, Record{wr});
    const auto  work_hash  = Crypto::hash_block(work_block);

    ConceptLink cl;
    cl.from.chain = ctx.user_id.bytes;
    cl.from.hash  = work_hash.bytes;
    cl.to         = parse_ref(pos[2]);
    cl.kind       = "исполняет";
    const Block link_block = append_record(ctx, Record{cl});

    std::cout << "работа: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(work_hash.bytes) << "  (" << wr.hours
              << "ч, привязана к потребности)\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) { upload_block(via, work_block); upload_block(via, link_block); }
    return 0;
}

// bc deal settle NEED_REF --via URL [--leaf L] [--yes]
// OFFERS to close the need. Accepting and paying is one record already (bc
// accept, records.md §9.5); a paid work ≠ need closed — multi-stage needs are
// real, so the owner is asked, never overruled (records.md §8.6).
static int cmd_deal_settle(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);
    const auto via = flag_val(argc, argv, "--via");
    if (pos.size() < 3 || via.empty()) {
        std::cerr << "Usage: bc deal settle NEED_REF --via URL [--leaf L] [--yes]\n";
        return 1;
    }
    const std::string& need_ref = pos[2];
    const auto deal = find_deal(via, need_ref);
    if (!deal) {
        std::cerr << "агрегатор не знает эту сделку — блоки выгружены (--via)?\n";
        return 1;
    }
    const NodeIndex leaf   = parse_leaf_index(argc, argv);
    const auto      me_hex = to_hex(load_user_id(data_dir).bytes);
    if (jstr(deal->object, "seeker") != me_hex) {
        std::cerr << "рассчитывается заказчик — это не ваша потребность\n";
        return 1;
    }

    // Приёмка и оплата — одна запись (bc accept); здесь остаётся только решить,
    // закрыта ли потребность.
    std::size_t paid_works = 0;
    if (const auto* ws = jarr(deal->object, "works"))
        for (const auto& w : ws->array) {
            const auto* accepted =
                w.is_object() ? records::json::find(w.object, "accepted") : nullptr;
            if (accepted && accepted->boolean) ++paid_works;
        }
    if (paid_works == 0)
        std::cout << "принятых и оплаченных работ нет — сначала bc accept\n";

    // offer to close — the owner decides, the tool never does
    std::string close_from;   // the acceptance the need was closed BY (records.md §8.6)
    if (const auto* ws = jarr(deal->object, "works"))
        for (const auto& w : ws->array)
            if (w.is_object() && !jstr(w.object, "acceptance_ref").empty())
                close_from = jstr(w.object, "acceptance_ref");
    if (close_from.empty()) close_from = need_ref;   // nothing to point at

    if (!flag_present(argc, argv, "--yes")) {
        std::cout << "Потребность закрыта? [y/N] " << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) ||
            (answer != "y" && answer != "Y" && answer != "д" && answer != "Д")) {
            std::cout << "оставлена открытой\n";
            return 0;
        }
    }
    Context     ctx(data_dir, leaf);
    ConceptLink cl;
    cl.from = parse_ref(close_from);
    cl.to   = parse_ref(need_ref);
    cl.kind = "закрыто";
    const Block block = append_record(ctx, Record{cl});
    upload_block(via, block);
    const auto slash = close_from.find('/');
    std::cout << "потребность закрыта (чем: "
              << short_hex_str(slash == std::string::npos
                                   ? close_from
                                   : close_from.substr(slash + 1)) << ")\n";
    return 0;
}

// bc apply --draft FILE [--dry-run] [--yes] [--leaf N] [--via URL]
//
// A scribe, an accountant or an AI prepares the draft; the owner signs it here
// (ИР-005 п.5). The draft holds no keys and grants no authority: value appears
// only when each record is appended to the owner's branch under the owner's key
// — one block, one signature. records::parse_draft refuses value-bearing types
// outright, so a Transfer cannot hide among profile facts; what remains is a
// short, fully-rendered batch a person can read in one sitting.
static int cmd_apply(const fs::path& data_dir, int argc, char** argv) {
    const auto file = flag_val(argc, argv, "--draft");
    if (file.empty()) {
        std::cerr << "Usage: bc apply --draft FILE [--dry-run] [--yes] "
                     "[--leaf N] [--via URL]\n";
        return 1;
    }

    std::ifstream f(file, std::ios::binary);
    if (!f) {
        std::cerr << "cannot read " << file << "\n";
        return 1;
    }
    const std::string json((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());

    records::Draft draft;
    try {
        draft = records::parse_draft(json);
    } catch (const records::DraftError& e) {
        std::cerr << "Черновик отклонён — " << e.what() << "\n";
        return 1;
    }

    // --leaf wins over the draft's own "leaf"; then the draft; then the default.
    NodeIndex leaf = parse_leaf_index(argc, argv);
    if (flag_val(argc, argv, "--leaf").empty() && draft.leaf.has_value())
        leaf = static_cast<NodeIndex>(*draft.leaf);

    // A model asked to pick a catalog slug sometimes invents one. An invented
    // slug does not fail loudly — the person is simply never found by anyone
    // searching for that skill. So it is checked before signing, not after.
    const auto via = flag_val(argc, argv, "--via");
    if (!flag_present(argc, argv, "--force")) {
        if (const auto catalogs = fetch_catalogs(via)) {
            bool ok = true;
            for (std::size_t i = 0; i < draft.records.size(); ++i)
                if (const auto* c = std::get_if<Concept>(&draft.records[i]))
                    ok &= validate_slugs(c->tags, *catalogs,
                                         "запись #" + std::to_string(i + 1));
            if (!ok) return 1;
        }
    }

    // Everything is rendered in full: hiding part of a record is the very risk
    // being guarded against.
    std::cout << "Черновик: " << file << "\n"
              << "Ветка:    " << leaf << "\n"
              << "Записей:  " << draft.records.size() << "\n\n";
    for (std::size_t i = 0; i < draft.records.size(); ++i)
        std::cout << "  " << (i + 1) << ". "
                  << records::render_record(draft.records[i]) << "\n\n";

    if (flag_present(argc, argv, "--dry-run")) {
        std::cout << "Пробный прогон — ничего не записано и не подписано.\n";
        return 0;
    }

    if (!flag_present(argc, argv, "--yes")) {
        std::cout << "Подписать " << draft.records.size()
                  << " запис(ь/и) своим ключом ветки " << leaf << "? [y/N] "
                  << std::flush;
        std::string answer;
        if (!std::getline(std::cin, answer) ||
            (answer != "y" && answer != "Y" && answer != "д" && answer != "Д")) {
            std::cout << "Отменено — ничего не подписано.\n";
            return 1;
        }
    }

    Context ctx(data_dir, leaf);
    for (std::size_t i = 0; i < draft.records.size(); ++i) {
        const Block block = append_record(ctx, draft.records[i]);
        std::cout << "  " << (i + 1) << ". block #" << block.address.block_index
                  << "  hash: " << to_hex(Crypto::hash_block(block).bytes) << "\n";
        if (!via.empty()) upload_block(via, block);
    }
    std::cout << "Подписано и записано: " << draft.records.size() << "\n";
    return 0;
}

// bc export profiles --via URL [--out FILE] [--chain UID_HEX]
// The aggregator's decoded self-descriptions (records.md §8.6, records.md §13): skills,
// needs, aspirations of every known chain, tags verbatim. This is the JSON that
// feeds manual need↔skill matching or an external AI — the project embeds none.
static int cmd_export_profiles(int argc, char** argv) {
    const auto via   = flag_val(argc, argv, "--via");
    const auto out   = flag_val(argc, argv, "--out");
    const auto chain = flag_val(argc, argv, "--chain");
    if (via.empty()) {
        std::cerr << "Usage: bc export profiles --via URL [--out FILE] "
                     "[--chain UID_HEX]\n";
        return 1;
    }
    const std::string path = chain.empty() ? "/profiles" : "/profiles/" + chain;

    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Get(path);
    if (!res) {
        std::cerr << "aggregator unreachable: " << via << "\n";
        return 1;
    }
    if (res->status != 200) {
        std::cerr << res->body << "\n";
        return 1;
    }
    if (out.empty()) {
        std::cout << res->body << "\n";
        return 0;
    }
    std::ofstream f(out, std::ios::binary);
    if (!f) {
        std::cerr << "cannot write " << out << "\n";
        return 1;
    }
    f << res->body;
    std::cerr << "wrote " << res->body.size() << " bytes to " << out << "\n";
    return 0;
}

// bc discover --via URL [--leaf L]
// Ranked merge-partner suggestions for the own chain (sync.md §8).
static int cmd_discover(const fs::path& data_dir, int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc discover --via URL\n";
        return 1;
    }
    const UserId me = load_user_id(data_dir);
    return economy_get(via, "/discovery/" + to_hex(me.bytes));
}

// bc pledge add --target REF --units N [--executor UID] [--expires TS] [--leaf L]
static int cmd_pledge_add(const fs::path& data_dir, int argc, char** argv) {
    const auto target_s = flag_val(argc, argv, "--target");
    const auto units_s  = flag_val(argc, argv, "--units");
    if (target_s.empty() || units_s.empty()) {
        std::cerr << "Usage: bc pledge add --target REF --units N "
                     "[--executor UID_HEX] [--expires UNIX_TS] [--leaf L]\n";
        return 1;
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    Pledge p{};
    p.target    = parse_ref(target_s);
    p.units     = std::stod(units_s);
    p.timestamp = static_cast<int64_t>(std::time(nullptr));
    if (p.units <= 0) throw std::runtime_error("--units must be positive");
    const auto exec_s = flag_val(argc, argv, "--executor");
    if (!exec_s.empty()) p.executor = uid_from_hex(exec_s);
    const auto exp_s = flag_val(argc, argv, "--expires");
    if (!exp_s.empty()) p.expires = static_cast<int64_t>(std::stoll(exp_s));

    const Block block = append_record(ctx, Record{p});
    std::cout << "pledge ref: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    // Transfer v4 (records.md §11.1): a pledge is closed by the deal that accepts and
    // pays for the work, naming the pledge in --pledge.
    std::cerr << "(settle with: bc accept --work <work ref> ... --pledge <pledge ref>; "
                 "revoke with: bc pledge revoke)\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc pledge revoke --pledge REF [--leaf L]
static int cmd_pledge_revoke(const fs::path& data_dir, int argc, char** argv) {
    const auto pledge_s = flag_val(argc, argv, "--pledge");
    if (pledge_s.empty()) {
        std::cerr << "Usage: bc pledge revoke --pledge REF [--leaf L]\n";
        return 1;
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    PledgeRevoke pr{};
    pr.pledge    = parse_ref(pledge_s);
    pr.timestamp = static_cast<int64_t>(std::time(nullptr));
    if (pr.pledge.chain != ctx.user_id.bytes)
        throw std::runtime_error("can only revoke an own pledge (records.md §11.4)");

    const Block block = append_record(ctx, Record{pr});
    std::cout << "revoke block #" << block.address.block_index << "  hash: "
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// bc pledge list [--leaf L]
// Own pledges with settlement status, computed from the own branch.
static int cmd_pledge_list(const fs::path& data_dir, int argc, char** argv) {
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    struct PledgeInfo {
        Pledge p;
        double settled = 0;
        bool   revoked = false;
    };
    std::map<std::string, PledgeInfo> pledges;   // own pledge block hash → info

    std::vector<Block> blocks;
    try { blocks = ctx.bc.get_branch(ctx.user_id, leaf); }
    catch (const BlockchainError&) {}
    for (const auto& b : blocks) {
        if (b.type != BlockType::DATA) continue;
        Record rec;
        try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
        catch (const CodecError&) { continue; }

        if (const auto* p = std::get_if<Pledge>(&rec)) {
            pledges[to_hex(Crypto::hash_block(b).bytes)] = PledgeInfo{*p, 0, false};
        } else if (const auto* pr = std::get_if<PledgeRevoke>(&rec)) {
            auto it = pledges.find(to_hex(pr->pledge.hash.data(), 32));
            if (it != pledges.end()) it->second.revoked = true;
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            // Settlement is named by `settles` (Transfer v4, records.md §11.1).
            if (!t->settles || t->settles->chain != ctx.user_id.bytes) continue;
            auto it = pledges.find(to_hex(t->settles->hash.data(), 32));
            if (it == pledges.end()) continue;
            for (const auto& o : t->origins) it->second.settled += o.units;
        }
    }

    if (pledges.empty()) {
        std::cout << "(no pledges)\n";
        return 0;
    }
    const auto now = static_cast<int64_t>(std::time(nullptr));
    for (const auto& [hash_hex, info] : pledges) {
        const auto& p = info.p;
        std::string status;
        if (info.settled + 1e-9 >= p.units) status = "SETTLED";
        else if (info.revoked)              status = "REVOKED";
        else if (p.expires && *p.expires < now) status = "EXPIRED";
        else                                status = "ACTIVE";

        std::cout << hash_hex.substr(0, 16) << "...  " << status
                  << "  " << info.settled << "/" << p.units << "h"
                  << "  target:" << to_hex(p.target.hash.data(), 32).substr(0, 16) << "...";
        if (p.executor) std::cout << "  exec:" << to_hex(p.executor->data(), 32).substr(0, 16) << "...";
        if (p.expires)  std::cout << "  expires:" << *p.expires;
        std::cout << "\n";
    }
    return 0;
}

// ── Seal commands ─────────────────────────────────────────────────────────────

// bc seal add BLOCK_HASH_HEX [--leaf L]
// bc seal add --mode open --idx BLOCK_IDX [--leaf L] [--user UID_HEX]
static int cmd_seal_add(const fs::path& data_dir, int argc, char** argv) {
    const bool open_mode = (flag_val(argc, argv, "--mode") == "open");
    const NodeIndex leaf  = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    SealManager sm(ctx.storage);

    Seal seal;
    if (open_mode) {
        const auto idx_s = flag_val(argc, argv, "--idx");
        if (idx_s.empty()) {
            std::cerr << "Usage: bc seal add --mode open --idx BLOCK_IDX [--leaf L] [--user UID_HEX]\n";
            return 1;
        }
        BlockIndex block_idx;
        try { block_idx = static_cast<BlockIndex>(std::stoul(idx_s)); }
        catch (...) { std::cerr << "Invalid --idx value\n"; return 1; }

        const auto user_s = flag_val(argc, argv, "--user");
        UserId target_user = ctx.user_id;
        if (!user_s.empty() && !from_hex(user_s, target_user.bytes.data(), 32)) {
            std::cerr << "Invalid --user hex\n";
            return 1;
        }

        BlockAddress addr{target_user, leaf, block_idx};
        const Block block = (target_user == ctx.user_id)
            ? ctx.storage.get_block(addr)
            : ctx.storage.get_external_block(addr);
        seal = sm.create_open_seal(block, ctx.working_kp);
    } else {
        const auto pos = get_positionals(argc, argv);  // [seal, add, <hash>]
        if (pos.size() < 3) {
            std::cerr << "Usage: bc seal add BLOCK_HASH_HEX [--leaf L]\n"
                         "       bc seal add --mode open --idx N [--leaf L] [--user UID]\n";
            return 1;
        }
        Hash block_hash{};
        if (!from_hex(pos[2], block_hash.bytes.data(), 32)) {
            std::cerr << "Invalid block hash hex (expected 64 hex chars)\n";
            return 1;
        }
        seal = sm.create_seal(block_hash, ctx.working_kp, SealMode::BLIND);
    }

    std::cout << "sealed:  " << to_hex(seal.block_hash.bytes) << "\n"
              << "signer:  " << to_hex(seal.signer_id.bytes)  << "\n"
              << "mode:    " << (seal.mode == SealMode::BLIND ? "BLIND" : "OPEN") << "\n";

    // Publish to the seal warehouse (sync.md §7.2) — witnessing only carries
    // weight when third parties can see it.
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) {
        httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
        cli.set_connection_timeout(5);
        const auto bytes = Serializer::encode(seal);
        const auto res   = cli.Post("/seals/" + to_hex(seal.block_hash.bytes),
            std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
            "application/cbor");
        std::cerr << ((res && res->status == 200) ? "Published to " : "publish failed: ")
                  << via << "\n";
    }
    return 0;
}

// bc seal list BLOCK_HASH_HEX
static int cmd_seal_list(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [seal, list, <hash>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc seal list BLOCK_HASH_HEX\n";
        return 1;
    }
    Hash block_hash{};
    if (!from_hex(pos[2], block_hash.bytes.data(), 32)) {
        std::cerr << "Invalid block hash hex (expected 64 hex chars)\n";
        return 1;
    }

    LmdbStorage storage(data_dir / "db");
    SealManager  sm(storage);
    auto         seals = sm.get_seals(block_hash);

    // --via: pull remote seals from the warehouse, verify each signature
    // (the warehouse is untrusted) and keep the new ones locally.
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) {
        httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
        cli.set_connection_timeout(5);
        const auto res = cli.Get("/seals/" + to_hex(block_hash.bytes));
        if (res && res->status == 200) {
            std::size_t imported = 0, rejected = 0;
            const std::string& body = res->body;
            std::size_t bpos = 0;
            auto head = [&](uint8_t expect_major) -> long long {
                if (bpos >= body.size()) return -1;
                const uint8_t b = static_cast<uint8_t>(body[bpos++]);
                if ((b >> 5) != expect_major) return -1;
                const uint8_t ai = b & 0x1F;
                if (ai < 24) return ai;
                int extra = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : -1;
                if (extra < 0 || bpos + extra > body.size()) return -1;
                long long v = 0;
                for (int i = 0; i < extra; ++i)
                    v = (v << 8) | static_cast<uint8_t>(body[bpos++]);
                return v;
            };
            const long long n = head(4);
            for (long long i = 0; i < n; ++i) {
                const long long len = head(2);
                if (len < 0 || bpos + len > body.size()) break;
                try {
                    const Seal s = Serializer::decode_seal(
                        reinterpret_cast<const uint8_t*>(body.data()) + bpos,
                        static_cast<size_t>(len));
                    const bool genuine =
                        s.block_hash == block_hash &&
                        Crypto::verify(s.block_hash.bytes.data(),
                                       s.block_hash.bytes.size(),
                                       s.signature, s.signer_id);
                    const bool known =
                        std::find(seals.begin(), seals.end(), s) != seals.end();
                    if (!genuine) ++rejected;
                    else if (!known) {
                        storage.put_seal(s);
                        seals.push_back(s);
                        ++imported;
                    }
                } catch (const std::exception&) {
                    ++rejected;
                }
                bpos += static_cast<std::size_t>(len);
            }
            std::cerr << "warehouse: " << imported << " new, " << rejected
                      << " rejected\n";
        } else {
            std::cerr << "warehouse unreachable: " << via << "\n";
        }
    }

    if (seals.empty()) {
        std::cout << "(no seals for this block)\n";
        return 0;
    }

    std::cout << seals.size() << " seal(s) for " << short_hex(block_hash.bytes) << ":\n\n";
    for (size_t i = 0; i < seals.size(); ++i) {
        const auto& s = seals[i];
        std::cout << "#" << i << "\n"
                  << "  signer: " << to_hex(s.signer_id.bytes) << "\n"
                  << "  mode:   " << (s.mode == SealMode::BLIND ? "BLIND" : "OPEN") << "\n"
                  << "  sig:    " << to_hex(s.signature.bytes) << "\n\n";
    }
    return 0;
}

static int cmd_list(const fs::path& data_dir, int argc, char** argv) {
    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    std::vector<Block> blocks;
    try {
        blocks = ctx.bc.get_branch(ctx.user_id, leaf);
    } catch (const BlockchainError&) {
        // branch may be empty
    }

    if (blocks.empty()) {
        std::ostringstream hint;
        hint << "bc concept add \"text\"";
        if (leaf != DEFAULT_LEAF) hint << " --leaf " << leaf;
        std::cout << "(no records yet — add one with: " << hint.str() << ")\n";
        return 0;
    }

    for (const auto& block : blocks) {
        const auto hash     = Crypto::hash_block(block);
        const auto hash_str = to_hex(hash.bytes);

        if (block.type != BlockType::DATA) {
            std::cout << "#" << block.address.block_index
                      << "  [non-data block type=" << static_cast<int>(block.type) << "]"
                      << "  hash:" << hash_str << "\n";
            continue;
        }

        std::string summary;
        try {
            const auto rec = Codec::decode(block.payload.data(), block.payload.size());
            summary = record_summary(rec);
        } catch (const CodecError& e) {
            summary = "[unknown payload: " + std::string(e.what()) + "]";
        }

        std::cout << "#" << block.address.block_index
                  << "  " << summary
                  << "  hash:" << hash_str << "\n";
    }
    return 0;
}

// ── Fraud / cache commands ────────────────────────────────────────────────────

// bc cache list
// Shows the persistent participant cache (sync.md §5): leaves and compositions.
static int cmd_cache_list(const fs::path& data_dir) {
    chainsync::ParticipantCache cache(data_dir / "sync_cache");

    const auto leaves = cache.leaves();
    std::cout << "leaves (" << leaves.size() << "):\n";
    for (const auto& entry : leaves) {
        const auto& rec = entry.second;
        std::cout << "  " << to_hex(entry.first.bytes) << "\n"
                  << "    chain: " << to_hex(rec.ref.address.user_id.bytes) << "\n"
                  << "    node: 0x" << std::hex << rec.ref.address.node_index
                  << std::dec << "  block: #" << rec.ref.address.block_index
                  << "\n    committed hash: " << to_hex(rec.ref.block_hash.bytes) << "\n";
    }
    const auto comps = cache.compositions();
    std::cout << "compositions (" << comps.size() << "):\n";
    for (const auto& entry : comps)
        std::cout << "  " << to_hex(entry.first.bytes) << "\n"
                  << "    = " << to_hex(entry.second.left_child.bytes) << "\n"
                  << "    + " << to_hex(entry.second.right_child.bytes) << "\n";
    return 0;
}

// bc cache publish --via URL
// Pushes the local participant cache to an aggregator warehouse (sync.md §7.1).
static int cmd_cache_publish(const fs::path& data_dir, int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc cache publish --via URL\n";
        return 1;
    }
    chainsync::ParticipantCache cache(data_dir / "sync_cache");
    chainsync::HttpSnapshotStore store(via);
    const std::size_t n = chainsync::publish_cache(store, cache);
    std::cout << "published " << n << " new entr(ies) of "
              << (cache.leaf_count() + cache.composition_count())
              << " to " << via << "\n";
    return 0;
}

// bc cache complete --via URL [--root HEX] [--leaf L]
// Pulls everything missing under a snapshot root from the warehouse into the
// local cache (sync.md §7.1). Default root: own branch's current snapshot.
static int cmd_cache_complete(const fs::path& data_dir, int argc, char** argv) {
    const auto via = flag_val(argc, argv, "--via");
    if (via.empty()) {
        std::cerr << "Usage: bc cache complete --via URL [--root HEX] [--leaf L]\n";
        return 1;
    }

    Hash root{};
    const auto root_hex = flag_val(argc, argv, "--root");
    if (!root_hex.empty()) {
        if (!from_hex(root_hex, root.bytes.data(), 32)) {
            std::cerr << "Invalid --root hex (expected 64 hex chars)\n";
            return 1;
        }
    } else {
        const NodeIndex leaf = parse_leaf_index(argc, argv);
        Context ctx(data_dir, leaf);
        MergeSession session(ctx.storage, ctx.validator);
        root = session.snapshot_for(ctx.user_id, leaf).merkle_root;
    }

    chainsync::ParticipantCache cache(data_dir / "sync_cache");
    chainsync::HttpSnapshotStore store(via);
    const std::size_t added = chainsync::complete_cache(store, cache, root);
    std::cout << "root:  " << to_hex(root.bytes) << "\n"
              << "added: " << added << " entr(ies)  (cache now "
              << cache.leaf_count() << " leaves / "
              << cache.composition_count() << " compositions)\n";
    return 0;
}

// bc fraud claim --kind (bad_sig|hash_mismatch) --target CHAIN/BLOCKHASH
//                --merkle-root HEX --leaf-hash HEX [--reason TEXT] [--leaf L]
// Builds a FraudClaim proof from the participant cache (sync.md §5.3, closes
// blockchain.md §11.9), checks it locally, and writes the claim record into the
// own chain (records.md §3A.1) only when the verdict is CONFIRMED — a refuted
// claim carries zero weight, a fabricated one penalizes the accuser.
// --target is the accused MERGE block, --merkle-root the root committed in its
// payload, --leaf-hash the accused participant leaf (see 'bc cache list').
static int cmd_fraud_claim(const fs::path& data_dir, int argc, char** argv) {
    const auto kind     = flag_val(argc, argv, "--kind");
    const auto target_s = flag_val(argc, argv, "--target");
    const auto root_s   = flag_val(argc, argv, "--merkle-root");
    const auto leaf_s   = flag_val(argc, argv, "--leaf-hash");
    if ((kind != "bad_sig" && kind != "hash_mismatch") ||
        target_s.empty() || root_s.empty() || leaf_s.empty()) {
        std::cerr << "Usage: bc fraud claim --kind (bad_sig|hash_mismatch) "
                     "--target CHAIN_HEX/BLOCKHASH_HEX --merkle-root HEX "
                     "--leaf-hash HEX [--reason TEXT] [--leaf L]\n";
        return 1;
    }
    Hash root{}, leaf_hash{};
    if (!from_hex(root_s, root.bytes.data(), 32)) {
        std::cerr << "Invalid --merkle-root (expected 64 hex chars)\n";
        return 1;
    }
    if (!from_hex(leaf_s, leaf_hash.bytes.data(), 32)) {
        std::cerr << "Invalid --leaf-hash (expected 64 hex chars)\n";
        return 1;
    }
    const Ref target = parse_ref(target_s);

    chainsync::ParticipantCache cache(data_dir / "sync_cache");
    const auto proof = cache.build_proof_bytes(root, leaf_hash);
    if (!proof) {
        std::cerr << "No proof material: the leaf is not cached, or no composition "
                     "chain links it\nto this root (see 'bc cache list').\n";
        return 1;
    }
    std::cout << "proof: " << to_hex(proof->data(), proof->size()) << "\n";

    const FraudVerdict v = FraudProof::verify(kind, proof->data(), proof->size(), root);
    if (v == FraudVerdict::REFUTED_HONEST) {
        std::cerr << "verdict: REFUTED_HONEST — the committed data shows no such defect.\n"
                     "Claim NOT written (it would carry zero weight).\n";
        return 1;
    }
    if (v == FraudVerdict::REFUTED_FABRICATED) {
        std::cerr << "verdict: REFUTED_FABRICATED — the proof does not hold up.\n"
                     "Claim NOT written (publishing it would penalize you as the accuser).\n";
        return 1;
    }
    std::cerr << "verdict: CONFIRMED — writing FraudClaim to own chain.\n";

    FraudClaim claim{target, kind, *proof, flag_val(argc, argv, "--reason")};
    return cmd_write(data_dir, argc, argv,claim);
}

// ── Usage ─────────────────────────────────────────────────────────────────────

// bc fraud verify --kind (bad_sig|hash_mismatch) --proof HEX --merkle-root HEX
// Checks a serialized FraudClaim proof against the accused block's committed root.
// Pure computation — needs no identity/branch. Prints the verdict.
static int cmd_fraud_verify(int argc, char** argv) {
    const auto kind    = flag_val(argc, argv, "--kind");
    const auto proof_s = flag_val(argc, argv, "--proof");
    const auto root_s  = flag_val(argc, argv, "--merkle-root");
    if (kind.empty() || proof_s.empty() || root_s.empty()) {
        std::cerr << "Usage: bc fraud verify --kind (bad_sig|hash_mismatch) "
                     "--proof HEX --merkle-root HEX\n";
        return 1;
    }

    const auto proof = from_hex_vec(proof_s);
    Hash merkle_root{};
    if (!from_hex(root_s, merkle_root.bytes.data(), 32)) {
        std::cerr << "Invalid --merkle-root (expected 64 hex chars)\n";
        return 1;
    }

    const FraudVerdict v =
        FraudProof::verify(kind, proof.data(), proof.size(), merkle_root);
    switch (v) {
        case FraudVerdict::CONFIRMED:
            std::cout << "verdict: CONFIRMED (fraud proven → strong negative on the accused)\n";
            break;
        case FraudVerdict::REFUTED_HONEST:
            std::cout << "verdict: REFUTED_HONEST (no defect; zero weight, no penalty)\n";
            break;
        case FraudVerdict::REFUTED_FABRICATED:
            std::cout << "verdict: REFUTED_FABRICATED (bogus proof; penalty to the accuser)\n";
            break;
    }
    return 0;
}

// ── Key revocation (blockchain.md §6.7) ──────────────────────────────────────

// POST a certificate to the aggregator revocation warehouse (sync.md §7.2).
static bool post_revocation_cert(const std::string& via, const UserId& chain,
                                 const std::vector<uint8_t>& bytes) {
    httplib::Client cli(via);  // scheme-aware: plain host:port, http:// or https://
    cli.set_connection_timeout(5);
    const auto res = cli.Post("/revocations/" + to_hex(chain.bytes),
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        "application/cbor");
    return res && res->status == 200;
}

// bc revoke create --node N [--ancestor A] [--since UNIX_TS] [--replace] [--out FILE]
// Writes a REVOCATION block into the ancestor's branch (default radius: parent)
// and emits a self-verifying certificate. --replace generates a fresh key and
// assigns it (the "second block" of the stop-then-replace flow when a stop
// revocation already exists).
static int cmd_revoke_create(const fs::path& data_dir, int argc, char** argv) {
    const auto node_s = flag_val(argc, argv, "--node");
    if (node_s.empty()) {
        std::cerr << "Usage: bc revoke create --node N [--ancestor A] "
                     "[--since UNIX_TS] [--replace] [--out FILE]\n";
        return 1;
    }
    const auto revoked = static_cast<NodeIndex>(std::stoull(node_s, nullptr, 0));
    if (revoked == 0)
        throw std::runtime_error("the root cannot be revoked — it has no ancestor "
                                 "(recovery: blockchain.md §11.5)");
    const auto anc_s = flag_val(argc, argv, "--ancestor");
    const NodeIndex ancestor = anc_s.empty()
        ? (revoked - 1) / 2  // default radius: parent (blockchain.md §6.7 rule 1)
        : static_cast<NodeIndex>(std::stoull(anc_s, nullptr, 0));

    const auto now     = static_cast<Timestamp>(std::time(nullptr));
    const auto since_s = flag_val(argc, argv, "--since");
    const Timestamp since =
        since_s.empty() ? now : static_cast<Timestamp>(std::stoll(since_s, nullptr, 0));

    LmdbStorage storage(data_dir / "db");
    Validator   validator(storage);
    Blockchain  bc(storage, validator);
    const UserId  uid         = load_user_id(data_dir);
    const KeyPair ancestor_kp = load_keypair(data_dir, ancestor);

    std::optional<KeyPair>   fresh;
    std::optional<PublicKey> replacement;
    if (flag_present(argc, argv, "--replace")) {
        fresh       = Crypto::generate_keypair();
        replacement = fresh->pub;
    }

    const Block b = bc.revoke_node(uid, revoked, since, replacement,
                                   ancestor, ancestor_kp, now);

    if (fresh.has_value()) {
        // The compromised key file steps aside; the branch continues fresh.
        const auto kp_path = key_path(data_dir, revoked);
        if (fs::exists(kp_path))
            fs::rename(kp_path, fs::path(kp_path.string() + ".revoked"));
        save_keypair(data_dir, revoked, *fresh);
    }

    std::cout << "revocation block #" << b.address.block_index
              << " in branch " << ancestor << "\n"
              << "revoked node " << revoked << "  since " << since
              << (replacement.has_value() ? "  — replacement assigned"
                                          : "  — emergency stop (branch frozen)")
              << "\n";

    const auto cert  = RevocationCert::build(storage, b.address);
    const auto bytes = Serializer::encode(cert);
    const auto out   = flag_val(argc, argv, "--out");
    if (!out.empty()) {
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        if (!f) throw std::runtime_error("cannot write " + out);
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
        std::cout << "certificate: " << out << " (" << bytes.size() << " bytes)\n";
    } else {
        std::cout << "certificate hex: " << to_hex(bytes.data(), bytes.size()) << "\n";
    }

    // Speed of propagation comes from the warehouse, not the record's height
    // (blockchain.md §6.7 rule 1) — publish right away when an aggregator is given.
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty())
        std::cerr << (post_revocation_cert(via, uid, bytes)
                          ? "Published to " : "publish failed: ")
                  << via << "\n";
    return 0;
}

// bc revoke publish --via URL (--node N | --cert FILE)
// Push certificates to the warehouse: every local REVOCATION block targeting
// --node (a certificate is built per block), or a ready certificate file.
static int cmd_revoke_publish(const fs::path& data_dir, int argc, char** argv) {
    const auto via    = flag_val(argc, argv, "--via");
    const auto node_s = flag_val(argc, argv, "--node");
    const auto file   = flag_val(argc, argv, "--cert");
    if (via.empty() || (node_s.empty() == file.empty())) {
        std::cerr << "Usage: bc revoke publish --via URL (--node N | --cert FILE)\n";
        return 1;
    }

    if (!file.empty()) {
        std::ifstream f(file, std::ios::binary);
        if (!f) { std::cerr << "cannot read " << file << "\n"; return 1; }
        f.seekg(0, std::ios::end);
        std::vector<uint8_t> bytes(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));

        const auto cert = Serializer::decode_revocation_cert(bytes.data(), bytes.size());
        RevocationCert::verify(cert); // don't publish garbage
        const bool ok = post_revocation_cert(via, cert.block.address.user_id, bytes);
        std::cout << (ok ? "published 1 certificate to " : "publish failed: ")
                  << via << "\n";
        return ok ? 0 : 1;
    }

    const auto node = static_cast<NodeIndex>(std::stoull(node_s, nullptr, 0));
    LmdbStorage storage(data_dir / "db");
    const UserId uid = load_user_id(data_dir);

    const auto addrs = storage.get_revocation_addresses(uid, node);
    if (addrs.empty()) {
        std::cout << "no local revocations target node " << node << "\n";
        return 1;
    }
    size_t ok = 0;
    for (const auto& addr : addrs) {
        const auto cert  = RevocationCert::build(storage, addr);
        const auto bytes = Serializer::encode(cert);
        if (post_revocation_cert(via, uid, bytes)) ++ok;
    }
    std::cout << "published " << ok << "/" << addrs.size()
              << " certificate(s) to " << via << "\n";
    return ok == addrs.size() ? 0 : 1;
}

// bc revoke fetch CHAIN_HEX --via URL
// Pull certificates for a chain from the warehouse, verify each autonomously
// (the warehouse is untrusted), import the good ones — path nodes plus the
// REVOCATION block (external) feed the local revocation index — and print the
// effective state per revoked node.
static int cmd_revoke_fetch(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [revoke, fetch, <chain>]
    const auto via = flag_val(argc, argv, "--via");
    if (pos.size() < 3 || via.empty()) {
        std::cerr << "Usage: bc revoke fetch CHAIN_HEX --via URL\n";
        return 1;
    }
    UserId chain{};
    if (!from_hex(pos[2], chain.bytes.data(), 32)) {
        std::cerr << "Invalid chain id hex (expected 64 hex chars)\n";
        return 1;
    }

    LmdbStorage storage(data_dir / "db");
    Validator   validator(storage);

    const auto r = fetch_import_revocations(storage, chain, via);
    if (!r.reachable) {
        std::cerr << "aggregator unreachable or error: " << via << "\n";
        return 1;
    }

    std::cout << "imported " << r.imported << " certificate(s), rejected "
              << r.rejected << "\n";
    for (NodeIndex node : r.touched) {
        const auto st = validator.effective_revocation(chain, node);
        if (!st.has_value()) continue;
        std::cout << "node " << node << ": revoked since "
                  << st->compromised_since
                  << (st->replacement_pubkey.has_value()
                          ? "  replacement " + short_hex(st->replacement_pubkey->bytes)
                          : std::string("  FROZEN"))
                  << "\n";
    }
    return 0;
}

// bc revoke verify (--cert FILE | --hex HEX)
// Autonomous check of a revocation certificate — needs no identity or storage.
static int cmd_revoke_verify(int argc, char** argv) {
    const auto file = flag_val(argc, argv, "--cert");
    const auto hexs = flag_val(argc, argv, "--hex");
    if (file.empty() == hexs.empty()) { // exactly one source required
        std::cerr << "Usage: bc revoke verify (--cert FILE | --hex HEX)\n";
        return 1;
    }

    std::vector<uint8_t> bytes;
    if (!file.empty()) {
        std::ifstream f(file, std::ios::binary);
        if (!f) { std::cerr << "cannot read " << file << "\n"; return 1; }
        f.seekg(0, std::ios::end);
        bytes.resize(static_cast<size_t>(f.tellg()));
        f.seekg(0);
        f.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    } else {
        bytes = from_hex_vec(hexs);
    }

    const auto cert = Serializer::decode_revocation_cert(bytes.data(), bytes.size());
    try {
        RevocationCert::verify(cert);
    } catch (const std::exception& e) {
        std::cout << "verdict: INVALID — " << e.what() << "\n";
        return 1;
    }
    const auto rp = RevocationCert::payload(cert);
    std::cout << "verdict:  OK\n"
              << "chain:    " << to_hex(cert.block.address.user_id.bytes) << "\n"
              << "author:   node " << cert.block.address.node_index
              << " (depth " << node_depth(cert.block.address.node_index) << ")\n"
              << "revoked:  node " << rp.revoked_node_index
              << "  key " << short_hex(rp.revoked_pubkey.bytes) << "\n"
              << "since:    " << rp.compromised_since << "\n"
              << "replaced: "
              << (rp.replacement_pubkey.has_value()
                      ? short_hex(rp.replacement_pubkey->bytes)
                      : std::string("no (emergency stop — branch frozen)"))
              << "\n";
    return 0;
}

// bc revoke status --node N
// Local view: effective revocation of a branch and per-block zone counters.
static int cmd_revoke_status(const fs::path& data_dir, int argc, char** argv) {
    const auto node_s = flag_val(argc, argv, "--node");
    if (node_s.empty()) {
        std::cerr << "Usage: bc revoke status --node N\n";
        return 1;
    }
    const auto node = static_cast<NodeIndex>(std::stoull(node_s, nullptr, 0));

    LmdbStorage storage(data_dir / "db");
    Validator   validator(storage);
    const UserId uid = load_user_id(data_dir);

    const auto st = validator.branch_revocation_status(uid, node);
    const char* state =
          st.state == BranchRevocationState::ACTIVE ? "ACTIVE"
        : st.state == BranchRevocationState::FROZEN ? "FROZEN" : "REPLACED";
    std::cout << "branch " << node << ": " << state << "\n";
    if (st.revocation.has_value()) {
        std::cout << "  since:  " << st.revocation->compromised_since << "\n"
                  << "  source: branch " << st.revocation->latest.node_index
                  << " block #" << st.revocation->latest.block_index << "\n";
        if (st.revocation->replacement_pubkey.has_value())
            std::cout << "  replacement: "
                      << short_hex(st.revocation->replacement_pubkey->bytes) << "\n";
    }
    size_t clean = 0, suspect = 0, invalid = 0;
    for (auto s : st.blocks) {
        if      (s == BlockRevocationStatus::CLEAN)   ++clean;
        else if (s == BlockRevocationStatus::SUSPECT) ++suspect;
        else                                          ++invalid;
    }
    std::cout << "  blocks: " << st.blocks.size() << " (clean " << clean
              << ", suspect " << suspect
              << ", invalid-after-replacement " << invalid << ")\n";
    if (validator.node_invalidated_by_revocation(uid, node))
        std::cout << "  node itself is invalidated by an ancestor's revocation "
                     "(blockchain.md §6.7 rule 6)\n";
    return 0;
}

static void print_usage() {
    std::cerr <<
R"(Usage: bc [--data-dir PATH] <command> [--leaf INDEX]

Identity:
  identity create                  Generate keys and create default branch
  identity show                    Show your User ID

Branches:
  branch init <leaf_index>         Init a new branch (decimal or 0x hex)
  block stub                       Append an empty stub block (bootstrap merge / time anchor)

Profile (records.md §8.6 — a profile fact is a tagged Concept, no new record
type; catalogs: docs/catalogs.md):
  concept add "text"               Declare a skill / need / aspiration:
    --tag kind:skill                 kind: skill | need | aspiration
    --tag cat:prof.electrician                | industry | hobby | obstacle
    [--tag horizon:now]              cat:  an entry from the catalogs
    [--tag retrain:yes]
  catalog --via URL                Browse the catalogs — find the cat: slug to
    [--search TEXT]                    write, instead of inventing one (records.md §8.7)
  export profiles --via URL        Dump decoded profiles of every known chain
    [--out FILE] [--chain UID_HEX]     (JSON: skills, needs, tags verbatim) —
                                       for need↔skill matching, by hand or by
                                       an external AI

Deals — from found match to settled labour (records.md §8.6; ИР-006):
  deal list --via URL [--all]      My deals, staged, with the exact next
                                       command — refs filled in, nothing copied
  deal take NEED_REF               «Берусь»: volunteer your skill for a need
    [--skill REF] [--via URL]          (auto-picks your only skill fact)
  deal hire NEED_REF               Hire a volunteer: a pledge naming them
    --executor UID --units N
  deal work NEED_REF               Log work AND attach it to the need in one
    --hours H --action TEXT            go («исполняет» link)
    [--title TEXT]                     «кем работал» — free text for people
    [--for CHAIN]                      the customer (default: the need's owner)
                                       — the result goes to them by this very
                                       record (records.md §9.4)
  deal settle NEED_REF --via URL   Pay accepted work (settling your pledge,
    [--yes]                            Transfer v4), then ASK to close — the
                                       owner decides, the tool never does

Matching — the point of it all (ИР-005):
  match --via URL                  Report: whose needs are closed by whose skills,
    [--json]                           exchange rings, deficits and who could
                                       retrain into them. Needs the aggregator to
                                       run with --catalog. --json → for an AI.

Finding each other (records.md §8.6):
  directory --skill SLUG --via URL Who offers this skill (closed facts excluded)
  needs list --via URL             Open needs: all of them, or one slug
    [--need SLUG]

  Unknown cat: slugs are REFUSED when the aggregator serves a catalog — a typo
  in a slug does not fail loudly, it just makes you unfindable. Override with
  --force when the catalog does not carry your slug yet.

Delegated authoring — a scribe/AI prepares, the OWNER signs (ИР-005 п.5):
  apply --draft FILE               Sign and append the records of a draft. The
    [--dry-run]                        draft holds no keys: it grants nothing
    [--yes] [--leaf N] [--via URL]     until signed here, one block per record.
                                       Only concept / concept-link / composite
                                       are accepted — value-bearing records are
                                       refused, so a transfer cannot hide in a
                                       batch. --dry-run renders it, signs nothing.

Knowledge graph:
  concept add <text>               Add an idea
              [--tag TAG...]
  concept link <from_ref> <to_ref> Link two ideas
               --kind KIND
  composite add <title>            Group ideas
               [--part REF...]
  copy <chain_hex>/<hash_hex>      Copy a record from another chain
  react <hash_hex>                 React to a record (-128..+127)
        --value N
        [--chain CHAIN_HEX]        (default: own chain)

Labor:
  work log                         Log a work event
    --action "description"
    --hours  FLOAT
    [--title TEXT]                     «кем работал» — free text for people
                                       («электрик, 5-й разряд»); nothing is
                                       computed on it (ИР-022 шаг 11)
    [--for CHAIN]                      the customer of an order agreed in
                                       advance: the result goes to them by
                                       this very record (records.md §9.4);
                                       otherwise — bc release afterwards
    [--start UNIX_TS]                  default: the work just ended; if those
                                       hours are claimed, the latest free
                                       window before — never the future
                                       (the goods thread, records.md §9.4)
    [--axis-share PATH=SHARE ...]      proposed breakdown of the hours by
                                       axis (Σ 1); the buyer confirms it
    [--input  NAME:QTY:UNIT ...]   (repeatable)
    [--output NAME:QTY:UNIT[:SHARE[:PRICE[:UNITS]]] ...]  a lot (repeatable,
                                       records.md §9.4): SHARE of the hours when
                                       several outputs; PRICE — the tag, labor-
                                       hours for the lot, or k=K; UNITS — the
                                       unit numbers, comma separated
    [--tool  HASH_PREFIX:USED ...] (repeatable) carry the cost of an own
    [--material HASH_PREFIX:QTY ...]   tool (tool-hours) or material batch
                                       (units) into this work (ИР-011): the
                                       CLI runs each carry thread itself
  accept                           Accept received work AND pay for it: one
                                       record, the payment equals the appraisal
                                       (records.md §9.5)
    --work        WORK_REF             (fetched from --via when missing)
    --quality     TEXT
    --axis PATH=HOURS ...              the breakdown is REQUIRED (ИР-022): hours
    | --axis-share PATH=SHARE ...       per axis (their sum is the price), shares
    | --like ACCEPT_REF                of the price, or «такая же работа, как
                                       та» — the sample's shares, kept as
                                       provenance; none given — the worker's
                                       proposal from the work record
    [--hours-raw FLOAT]                default: hours of the fetched WorkRecord
    [--via URL] [--k FLOAT]            price = hours × Σ network prices of the
                                       axes named × k (economy.md §2а)
    [--coef FLOAT] [--labor-units F]   manual rate / full manual appraisal
                                       carried_units is derived automatically
                                       from the work's carry — never by hand
    [--pledge REF]                     the own pledge this deal closes
    [--buyback-of REF]                 buy back on a pre-signed return
    [--release REF]                    the seller's release to you — see
                                       records.md §9.8; found at the aggregator
                                       by default. A deal the seller did not
                                       address (no customer, no live release)
                                       is void, and is refused (records.md §9.5)
  release WORK --to CHAIN          Give the result of an own work to a buyer
    [--days N | --until UNIX_TS]       (records.md §9.8): only their deal counts,
    [--via URL]                        and only before the term (default 7 days)
    [--qty Q | --units N1,N2 [--out N]] a portion of lot N at its price tag —
                                       a link of the lot's sub-thread; never
                                       more than it holds, a unit sold once
    [--buyback | --buyback-to CHAIN]   an addressed buyout (records.md §9.8):
    [--buyback-days N]                 until the term the buyer sells it on
                                       only to the addressee, and pre-signs
                                       the return in the purchase
    [--rent]                           a rent: back to oneself at what is left
                                       after the wear by the service life —
                                       nothing above the wear is paid
  writeoff WORK --qty Q --reason T  Write off part of a lot — spoiled, lost,
    [--out N] [--via URL]              used up (records.md §9.9); WORK may be
                                       an own purchase
  release PURCHASE --to CHAIN --qty Q  Resale (records.md §9.8): a portion of an
                                       own purchase at its share of the price
                                       paid — no markup; the serial stays the
                                       maker's
  stock [CHAIN] --via URL          What was bought and may be sold on: lots,
                                       sold, written off, in stock
  thread [CHAIN] --via URL         The goods thread (records.md §9.4): works in
                                       time, faults (outside the thread, broken
                                       links, hours sold twice), labor not yet
                                       confirmed — by the proposed axes

Means of production (ИР-011, records.md §10.2, records.md §9.4):
  tool add ИМЯ --cost H --life HRS Register a tool: cost in labor-hours,
    [--serial S] [--desc T]            design life in tool-hours. Without
    [--note "расчёт оценки"]           --paid the cost is the owner's estimate
    [--paid ACCEPT_REF]                (est) — give --note with the arithmetic.
    [--origin TOOL_REF]                Reissue: resale / downward revaluation /
                                       re-entry; cost ≤ previous remainder.
  material add ИМЯ --unit U        Register a consumables batch: cost in
    --cost H --qty Q                   labor-hours, size in units. Spent by
    [--note "расчёт"] [--paid REF]     --material in work log; est needs --note.
    [--origin MAT_REF]
  tool list / material list        Own tools/batches: cost, collected, remainder
  tool show / material show PREFIX Audit one carry thread: invariant, forks,
                                       formula, resale ceiling (economy.md §5б)
  rates --via URL                  Today's network prices of axes (signed
                                       DailyAggregate): what an hour of each
                                       thing work is made of pays, and W
  cloud --via URL [--coords]       Work cloud (ИР-022): samples of work others
                                       repeated with accept --like, each placed
                                       by what its family was paid for; the
                                       neighbours are work paid for the same
                                       things. Signed WorkCloud; --coords adds
                                       the 2D map
  axis-define --slug ОСЬ           Завести ось в своей цепи (ИР-022). Словарь труда
    --ru "Название"                    решают люди: заводить волен каждый, сила — от
    --description TEXT                 употребления. Отличает хорошую ось от плохой
                                       только аргумент; ось без описания это видно
  axis-ledger --via URL [--rent]   Куда труд сети ушёл НА САМОМ ДЕЛЕ: доля всех
                                       оплаченных часов по каждой оси, разброс цены
                                       и отметка «осью пользуются без описания».
                                       --rent: карта ренты — что корзина заплатила
                                       сверх сетевой цены названных осей и КТО
  fetch <chain>/<hash> --via URL   Fetch any foreign block for local reading

Merge over a relay (sync.md §4.1):
  merge run                        Initiate a merge and drive it to completion
    --peer UID_HEX                     partner's User ID
    --via  URL                         aggregator relay, e.g. https://host or http://host:8080
    [--depth N] [--timeout SEC]        declared depth (1) / give-up time (60)
  discover --via URL               Ranked merge-partner suggestions (sync.md §8)
  merge own --with NODE            Internal merge of two own branches: the
    [--leaf L]                         --leaf branch (public vertex) commits NODE
  merge serve --via URL            Respond to incoming merge OFFERs in a loop
    [--depth N] [--timeout SEC]        SEC=0: run forever
    [--once]                           exit after the first completed merge

Merge, manual relay (blockchain.md §6.4 bilateral two-round protocol):
  merge prepare                    Step 1: print own tip + snapshot blobs (send to partner)
  merge create --peer-tip HEX      Step 2: verify tip, union snapshots, create draft
               --peer-snapshot HEX          print draft_hash [--depth N: declared depth]
  merge cosign --draft HASH        Step 3: co-sign partner's draft_hash, print co_signature
  merge finalize --co-sig HEX      Step 4: attach partner co_sig, complete merge block

Seals (blockchain.md §7; gossip sync.md §7.2):
  seal add BLOCK_HASH_HEX          Create a BLIND seal on a block hash
    [--via URL]                        (--via publishes it to the warehouse)
  seal add --mode open             Create an OPEN seal (loads block from storage)
           --idx BLOCK_IDX
           [--user UID_HEX] [--via URL]
  seal list BLOCK_HASH_HEX         List seals for a block; --via pulls remote
    [--via URL]                        ones, verifying every signature locally

Fraud (records.md §3A, blockchain.md §6.5.6):
  fraud claim --kind KIND          Build a proof from the sync cache and write a
              --target REF                FraudClaim record (only when CONFIRMED)
              --merkle-root HEX           KIND = bad_sig | hash_mismatch
              --leaf-hash HEX             REF = accused merge block <chain>/<hash>
              [--reason TEXT]             (leaf hashes: bc cache list)
  fraud verify --kind KIND         Verify a FraudClaim proof against a committed root
               --proof HEX                 KIND = bad_sig | hash_mismatch
               --merkle-root HEX

Key revocation (blockchain.md §6.7; warehouse — sync.md §7.2):
  revoke create --node N           Revoke a branch key from an ancestor's branch
    [--ancestor A] [--since TS]        (default ancestor: parent). --replace
    [--replace] [--out FILE]           generates and assigns a fresh key; emits
    [--via URL]                        a self-verifying certificate (CBOR);
                                       --via publishes it to the warehouse
  revoke verify (--cert FILE | --hex HEX)  Check a certificate autonomously
  revoke status --node N           Effective revocation + per-block zones
  revoke publish --via URL         Push certificates to the warehouse
    (--node N | --cert FILE)
  revoke fetch CHAIN_HEX --via URL Pull, verify and import a chain's certificates

Economy (records.md §11 — именные трудочасы, records.md §12.7):
  wallet [--leaf N]                Branch purse (economy.md §5а: the --leaf
                                       branch's holdings) + chain-wide debt
  transfer recv <chain>/<hash>     Fetch an incoming deal (bc accept of your work),
    --via URL                          store it and acknowledge on-chain (Copy);
                                       a deal you did not address is refused
  pledge add --target REF          Promise labor-hours for a work/idea
    --units N [--executor UID_HEX] [--expires UNIX_TS]
  pledge revoke --pledge REF       Revoke the unsettled remainder of an own pledge
  pledge list                      Own pledges with settlement status
  ideas top --via URL              Funding board: pledged labor per idea (JSON)
  chain info UID_HEX --via URL     Economic dossier of a chain (JSON)
  trust CHAIN_HEX_OR_PREFIX        Economic footprint (ИР-010). Layer 1: credit
    [--via URL]                        history off the emission thread, computed
                                       locally, trusting nobody. With --via also
                                       layers 2–3 from the aggregator: who holds
                                       its paper (and how many of them YOU can
                                       reach), whose work it earned back, and how
                                       much of its turnover ever left its circle.
                                       Every figure carries its forging price;
                                       no score is ever computed.

Sync cache (sync.md §5; gossip sync.md §7.1):
  cache list                       List cached participant leaves and compositions
  cache publish --via URL          Push the local cache to an aggregator warehouse
  cache complete --via URL         Pull everything missing under a snapshot root
                 [--root HEX]          (default: own branch's current snapshot)

Other:
  list                             List all records in a branch

--leaf INDEX  Target branch node index (decimal or 0x hex) — branches may grow
              from ANY tree node (blockchain.md §3.2): low indices = high-level
              service branches, deep ones = personal/channel branches.
              Default: 0x7FFFFFFF. Valid range: 0..0xFFFFFFFE.
              Run 'branch init <index>' before first use of a new branch.

REF format: <chain_id_64hex>/<block_hash_64hex>
Default --data-dir: ~/.blockchain
)";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(); return 1; }

    const auto data_dir_s = flag_val(argc, argv, "--data-dir");
    fs::path data_dir;
    if (!data_dir_s.empty()) {
        data_dir = data_dir_s;
    } else {
        const char* home = std::getenv("HOME");
        data_dir = home ? fs::path(home) / ".blockchain" : fs::path(".blockchain");
    }

    const auto pos = get_positionals(argc, argv);
    if (pos.empty()) { print_usage(); return 1; }
    const auto& cmd    = pos[0];
    const auto  subcmd = pos.size() > 1 ? pos[1] : std::string{};

    try {
        if      (cmd == "identity"  && subcmd == "create")  return cmd_identity_create(data_dir);
        else if (cmd == "identity"  && subcmd == "show")    return cmd_identity_show(data_dir);
        else if (cmd == "concept"   && subcmd == "add")     return cmd_concept_add(data_dir, argc, argv);
        else if (cmd == "concept"   && subcmd == "link")    return cmd_concept_link(data_dir, argc, argv);
        else if (cmd == "composite" && subcmd == "add")     return cmd_composite_add(data_dir, argc, argv);
        else if (cmd == "copy")                             return cmd_copy(data_dir, argc, argv);
        else if (cmd == "react")                            return cmd_react(data_dir, argc, argv);
        else if (cmd == "work"      && subcmd == "log")     return cmd_work_log(data_dir, argc, argv);
        else if (cmd == "accept")                           return cmd_accept(data_dir, argc, argv);
        else if (cmd == "release")                          return cmd_release(data_dir, argc, argv);
        else if (cmd == "thread")                           return cmd_thread(data_dir, argc, argv);
        else if (cmd == "writeoff")                         return cmd_writeoff(data_dir, argc, argv);
        else if (cmd == "stock")                            return cmd_stock(data_dir, argc, argv);
        else if (cmd == "tool"      && subcmd == "add")     return cmd_tool_add(data_dir, argc, argv);
        else if (cmd == "tool"      && subcmd == "list")    return cmd_tool_list(data_dir, argc, argv);
        else if (cmd == "tool"      && subcmd == "show")    return cmd_tool_show(data_dir, argc, argv);
        else if (cmd == "material"  && subcmd == "add")     return cmd_material_add(data_dir, argc, argv);
        else if (cmd == "material"  && subcmd == "list")    return cmd_tool_list(data_dir, argc, argv);
        else if (cmd == "material"  && subcmd == "show")    return cmd_tool_show(data_dir, argc, argv);
        else if (cmd == "branch"    && subcmd == "init")    return cmd_branch_init(data_dir, argc, argv);
        else if (cmd == "block"     && subcmd == "stub")    return cmd_block_stub(data_dir, argc, argv);
        else if (cmd == "list")                             return cmd_list(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "prepare") return cmd_merge_prepare(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "create")  return cmd_merge_create(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "cosign")  return cmd_merge_cosign(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "finalize")return cmd_merge_finalize(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "run")     return cmd_merge_run(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "own")     return cmd_merge_own(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "serve")   return cmd_merge_serve(data_dir, argc, argv);
        else if (cmd == "seal"      && subcmd == "add")     return cmd_seal_add(data_dir, argc, argv);
        else if (cmd == "seal"      && subcmd == "list")    return cmd_seal_list(data_dir, argc, argv);
        else if (cmd == "fraud"     && subcmd == "verify")  return cmd_fraud_verify(argc, argv);
        else if (cmd == "fraud"     && subcmd == "claim")   return cmd_fraud_claim(data_dir, argc, argv);
        else if (cmd == "revoke"    && subcmd == "create")  return cmd_revoke_create(data_dir, argc, argv);
        else if (cmd == "revoke"    && subcmd == "verify")  return cmd_revoke_verify(argc, argv);
        else if (cmd == "revoke"    && subcmd == "status")  return cmd_revoke_status(data_dir, argc, argv);
        else if (cmd == "revoke"    && subcmd == "publish") return cmd_revoke_publish(data_dir, argc, argv);
        else if (cmd == "revoke"    && subcmd == "fetch")   return cmd_revoke_fetch(data_dir, argc, argv);
        else if (cmd == "cache"     && subcmd == "list")    return cmd_cache_list(data_dir);
        else if (cmd == "cache"     && subcmd == "publish") return cmd_cache_publish(data_dir, argc, argv);
        else if (cmd == "cache"     && subcmd == "complete")return cmd_cache_complete(data_dir, argc, argv);
        else if (cmd == "wallet")                           return cmd_wallet(data_dir, argc, argv);
        else if (cmd == "trust")                            return cmd_trust(data_dir, argc, argv);
        else if (cmd == "ideas"     && subcmd == "top")     return cmd_ideas_top(argc, argv);
        else if (cmd == "rates")                            return cmd_rates(argc, argv);
        else if (cmd == "cloud")                            return cmd_cloud(argc, argv);
        else if (cmd == "axis-ledger")                      return cmd_axis_ledger(argc, argv);
        else if (cmd == "axis-define")                      return cmd_axis_define(data_dir, argc, argv);
        else if (cmd == "discover")                         return cmd_discover(data_dir, argc, argv);
        else if (cmd == "chain"     && subcmd == "info")    return cmd_chain_info(argc, argv);
        else if (cmd == "export"    && subcmd == "profiles")return cmd_export_profiles(argc, argv);
        else if (cmd == "apply")                            return cmd_apply(data_dir, argc, argv);
        else if (cmd == "catalog")                          return cmd_catalog(argc, argv);
        else if (cmd == "match")                            return cmd_match(argc, argv);
        else if (cmd == "deal"      && subcmd == "list")    return cmd_deal_list(data_dir, argc, argv);
        else if (cmd == "deal"      && subcmd == "take")    return cmd_deal_take(data_dir, argc, argv);
        else if (cmd == "deal"      && subcmd == "hire")    return cmd_deal_hire(data_dir, argc, argv);
        else if (cmd == "deal"      && subcmd == "work")    return cmd_deal_work(data_dir, argc, argv);
        else if (cmd == "deal"      && subcmd == "settle")  return cmd_deal_settle(data_dir, argc, argv);
        else if (cmd == "directory")                        return cmd_directory(argc, argv);
        else if (cmd == "needs"     && subcmd == "list")    return cmd_needs_list(argc, argv);
        else if (cmd == "fetch")                            return cmd_fetch(data_dir, argc, argv);
        else if (cmd == "pay" || (cmd == "transfer" && subcmd == "send")) {
            std::cerr << "снято: часы двигаются только сделкой — приёмка и оплата одной "
                         "записью, bc accept (records.md §9.5)\n";
            return 1;
        }
        else if (cmd == "transfer"  && subcmd == "recv")    return cmd_transfer_recv(data_dir, argc, argv);
        else if (cmd == "pledge"    && subcmd == "add")     return cmd_pledge_add(data_dir, argc, argv);
        else if (cmd == "pledge"    && subcmd == "revoke")  return cmd_pledge_revoke(data_dir, argc, argv);
        else if (cmd == "pledge"    && subcmd == "list")    return cmd_pledge_list(data_dir, argc, argv);
        else {
            std::cerr << "Unknown command: " << cmd;
            if (!subcmd.empty()) std::cerr << " " << subcmd;
            std::cerr << "\n\n";
            print_usage();
            return 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
}
