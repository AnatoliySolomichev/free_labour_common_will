#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/fraud.h>
#include <blockchain/merge_session.h>
#include <blockchain/seal_manager.h>
#include <blockchain/serializer.h>
#include <blockchain/storage.h>
#include <blockchain/validator.h>
#include <records/codec.h>
#include <records/types.h>
#include <sync/dialogue_channel.h>
#include <sync/participant_cache.h>
#include <sync/snapshot_exchange.h>

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
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
    "--agent", "--action", "--hours", "--start", "--input", "--output",
    "--work", "--quality", "--hours-raw", "--labor-units",
    "--peer-tip", "--peer-snapshot", "--draft", "--co-sig", "--mode", "--user",
    "--idx", "--depth", "--proof", "--merkle-root", "--target", "--leaf-hash",
    "--reason", "--peer", "--via", "--timeout",
    "--to", "--units", "--origin", "--pledge", "--executor", "--expires",
    "--acceptance", "--coef",
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
    if (v > 0xFFFF'FFFEu || !is_leaf_node(static_cast<NodeIndex>(v)))
        throw std::runtime_error(
            "--leaf " + s + " is not a valid leaf (depth must be 31, range 0x7FFFFFFF..0xFFFFFFFE)");
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

// ── ResourceQty parser ("name:qty:unit") ─────────────────────────────────────

static ResourceQty parse_rq(const std::string& s) {
    const auto p1 = s.find(':');
    if (p1 == std::string::npos) throw std::runtime_error("bad resource format: " + s);
    const auto p2 = s.find(':', p1 + 1);
    if (p2 == std::string::npos) throw std::runtime_error("bad resource format: " + s);
    return { s.substr(0, p1), std::stod(s.substr(p1 + 1, p2 - p1 - 1)), s.substr(p2 + 1) };
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
        } else if constexpr (std::is_same_v<T, Specialty>) {
            ss << "[Specialty]   \"" << r.name << "\"";
        } else if constexpr (std::is_same_v<T, Grade>) {
            ss << "[Grade]       spec:" << short_hex(r.specialty.hash)
               << "  level:" << static_cast<int>(r.level);
        } else if constexpr (std::is_same_v<T, Worker>) {
            ss << "[Worker]      chain:" << short_hex(r.chain);
        } else if constexpr (std::is_same_v<T, WorkRecord>) {
            ss << "[WorkRecord]  \"" << r.action << "\"  " << r.hours << "h";
            if (!r.inputs.empty())  ss << "  in:"  << r.inputs.size();
            if (!r.outputs.empty()) ss << "  out:" << r.outputs.size();
        } else if constexpr (std::is_same_v<T, Acceptance>) {
            ss << "[Acceptance]  work:" << short_hex(r.work.hash)
               << "  " << r.labor_units << " labor-h"
               << "  quality:" << r.quality;
        } else if constexpr (std::is_same_v<T, records::Transfer>) {
            double total = 0;
            for (const auto& o : r.origins) total += o.units;
            ss << "[Transfer]    to:" << short_hex(r.to)
               << "  " << total << "h  (" << r.origins.size() << " portion(s))";
            if (r.reason) ss << "  reason:" << short_hex(r.reason->hash);
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
    LmdbStorage storage;
    Validator   validator;
    Blockchain  bc;
    UserId      user_id;
    KeyPair     working_kp;
    NodeIndex   leaf;

    Context(const fs::path& data_dir, NodeIndex leaf_index)
        : storage   (data_dir / "db")
        , validator (storage)
        , bc        (storage, validator)
        , user_id   (load_user_id(data_dir))
        , working_kp(load_keypair(data_dir, leaf_index))
        , leaf      (leaf_index)
    {}
};

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
    std::string host = via;
    if (host.rfind("http://", 0) == 0) host = host.substr(7);
    httplib::Client cli(host);
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

// bc block stub [--leaf L]
// Appends an empty stub DATA block: bootstrap for merge (§6.4) or time anchor (§5.4).
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
        std::cerr << "Usage: bc branch init <leaf_index>\n"
                     "  leaf_index: decimal or hex (0x...), range 0x7FFFFFFF..0xFFFFFFFE\n";
        return 1;
    }

    NodeIndex leaf = 0;
    try {
        unsigned long long v = std::stoull(pos[2], nullptr, 0);
        if (v > 0xFFFF'FFFEu || !is_leaf_node(static_cast<NodeIndex>(v)))
            throw std::runtime_error("not a leaf");
        leaf = static_cast<NodeIndex>(v);
    } catch (...) {
        std::cerr << "Invalid leaf index: " << pos[2]
                  << " (must be at depth 31, range 0x7FFFFFFF..0xFFFFFFFE)\n";
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

static int cmd_concept_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [concept, add, <text>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc concept add <text> [--tag TAG...]\n";
        return 1;
    }
    Concept c;
    c.text = pos[2];
    c.tags = flag_all(argc, argv, "--tag");
    return cmd_write(data_dir, argc, argv,c);
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

static int cmd_specialty_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [specialty, add, <name>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc specialty add <name>\n";
        return 1;
    }
    return cmd_write(data_dir, argc, argv,Specialty{ pos[2] });
}

static int cmd_grade_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [grade, add, <specialty_ref>, <level>]
    if (pos.size() < 4) {
        std::cerr << "Usage: bc grade add <specialty_chain/hash> <level 1-6>\n";
        return 1;
    }
    const int level = std::stoi(pos[3]);
    if (level < 1 || level > 6) {
        std::cerr << "Level must be 1–6\n";
        return 1;
    }
    Grade g;
    g.specialty = parse_ref(pos[2]);
    g.level     = static_cast<uint8_t>(level);
    return cmd_write(data_dir, argc, argv,g);
}

static int cmd_work_log(const fs::path& data_dir, int argc, char** argv) {
    const auto agent_s  = flag_val(argc, argv, "--agent");
    const auto action_s = flag_val(argc, argv, "--action");
    const auto hours_s  = flag_val(argc, argv, "--hours");
    if (agent_s.empty() || action_s.empty() || hours_s.empty()) {
        std::cerr << "Usage: bc work log\n"
                     "    --agent  GRADE_CHAIN/HASH\n"
                     "    --action \"description\"\n"
                     "    --hours  FLOAT\n"
                     "   [--start  UNIX_TS]         (default: now)\n"
                     "   [--input  NAME:QTY:UNIT]   (repeatable)\n"
                     "   [--output NAME:QTY:UNIT]   (repeatable)\n";
        return 1;
    }
    WorkRecord wr;
    wr.agent  = parse_ref(agent_s);
    wr.action = action_s;
    wr.hours  = std::stod(hours_s);
    const auto start_s = flag_val(argc, argv, "--start");
    wr.start_ts = start_s.empty()
                  ? static_cast<int64_t>(std::time(nullptr))
                  : static_cast<int64_t>(std::stoll(start_s));
    for (const auto& s : flag_all(argc, argv, "--input"))  wr.inputs.push_back(parse_rq(s));
    for (const auto& s : flag_all(argc, argv, "--output")) wr.outputs.push_back(parse_rq(s));
    return cmd_write(data_dir, argc, argv,wr);
}

static int cmd_accept(const fs::path& data_dir, int argc, char** argv) {
    const auto work_s    = flag_val(argc, argv, "--work");
    const auto quality_s = flag_val(argc, argv, "--quality");
    const auto raw_s     = flag_val(argc, argv, "--hours-raw");
    const auto lu_s      = flag_val(argc, argv, "--labor-units");
    if (work_s.empty() || quality_s.empty()) {
        std::cerr << "Usage: bc accept\n"
                     "    --work         WORK_CHAIN/HASH\n"
                     "    --quality      TEXT\n"
                     "    [--hours-raw   FLOAT]   default: hours of the fetched WorkRecord\n"
                     "    [--coef        FLOAT]   grade coefficient, default 1.0\n"
                     "    [--labor-units FLOAT]   default: hours-raw * coef\n";
        return 1;
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    Acceptance a;
    a.work      = parse_ref(work_s);
    a.quality   = quality_s;
    a.timestamp = static_cast<int64_t>(std::time(nullptr));
    a.receiver  = ctx.user_id.bytes;

    if (raw_s.empty() || lu_s.empty()) {
        // Derive the appraisal from the worker's record (records.md §9.5).
        const auto block = find_external_by_hash(ctx, a.work.hash);
        if (!block || block->type != BlockType::DATA)
            throw std::runtime_error(
                "work record not fetched — run 'bc fetch <ref> --via URL' first, "
                "or pass --hours-raw and --labor-units");
        const auto rec = Codec::decode(block->payload.data(), block->payload.size());
        const auto* wr = std::get_if<WorkRecord>(&rec);
        if (!wr) throw std::runtime_error("--work does not reference a WorkRecord");
        const double coef = std::stod(flag_val(argc, argv, "--coef", "1.0"));
        a.hours_raw   = raw_s.empty() ? wr->hours : std::stod(raw_s);
        a.labor_units = lu_s.empty() ? a.hours_raw * coef : std::stod(lu_s);
    } else {
        a.hours_raw   = std::stod(raw_s);
        a.labor_units = std::stod(lu_s);
    }

    const Block block = append_record(ctx, Record{a});
    std::cout << "acceptance ref: " << to_hex(ctx.user_id.bytes) << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";
    std::cerr << "appraised: " << a.labor_units << " labor-h  (raw " << a.hours_raw
              << "h)\n(pay with: bc pay --acceptance <ref>)\n";
    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
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
    // snapshot with the union, so capture both for the cache fill (§5.2).
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
        // (sync.md §5.2) — the raw material for fraud claims (§11.9).
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
        upload_block(via, *dialogue.merge_block());   // feeds discovery (§8)
        return 0;
    }
    if (dialogue.failed()) { std::cerr << "merge failed: " << dialogue.error() << "\n"; return 1; }
    std::cerr << "no answer within " << timeout
              << "s — dialogue stalled and abandoned (safe to retry)\n";
    return 1;
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
                upload_block(via, *d.merge_block());   // feeds discovery (§8)
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

// ── Economy: named labor-hours (records.md §11, §12.7) ────────────────────────

static std::array<uint8_t, 32> uid_from_hex(const std::string& s) {
    std::array<uint8_t, 32> a{};
    if (!from_hex(s, a.data(), 32))
        throw std::runtime_error("invalid UserId hex (expected 64 hex chars): " + s);
    return a;
}

// Analytic wallet view — no protocol enforcement (records.md §12.7): own
// outgoing transfers come from the branch, incoming ones from the external
// store (fetched by 'bc transfer recv').
struct WalletView {
    std::map<std::string, double> holdings;   // issuer hex → held units of their paper
    double issued   = 0;                      // own paper put into circulation
    double redeemed = 0;                      // own paper returned (annihilated)
    double debt() const { return issued - redeemed; }
};

static WalletView compute_wallet(Context& ctx) {
    WalletView w;
    const auto& me = ctx.user_id.bytes;

    std::vector<Block> blocks;
    try { blocks = ctx.bc.get_branch(ctx.user_id, ctx.leaf); }
    catch (const BlockchainError&) {}
    for (const auto& b : blocks) {
        if (b.type != BlockType::DATA) continue;
        try {
            const auto rec = Codec::decode(b.payload.data(), b.payload.size());
            const auto* t  = std::get_if<records::Transfer>(&rec);
            if (!t || t->from != me) continue;
            for (const auto& o : t->origins) {
                if (o.issuer == me) w.issued += o.units;              // self-issue
                else w.holdings[to_hex(o.issuer.data(), 32)] -= o.units;
            }
        } catch (const CodecError&) {}
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
                else w.holdings[to_hex(o.issuer.data(), 32)] += o.units;
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

// Best-effort upload so the receiver can fetch the block from the aggregator.
static void upload_block(const std::string& via, const Block& block) {
    std::string host = via;
    if (host.rfind("http://", 0) == 0) host = host.substr(7);
    httplib::Client cli(host);
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

    double held = 0;
    std::cout << "holdings (paper of others):\n";
    if (w.holdings.empty()) std::cout << "  (none)\n";
    for (const auto& [issuer, units] : w.holdings) {
        std::cout << "  " << issuer.substr(0, 16) << "...  " << units << "h\n";
        held += units;
    }
    std::cout << "total held: " << held << "h\n"
              << "own debt in circulation: " << w.debt()
              << "h  (issued " << w.issued << ", redeemed " << w.redeemed << ")\n";
    return 0;
}

// bc transfer send --to UID --units N [--origin ISSUER_HEX:UNITS]...
//                  [--reason REF] [--via URL] [--leaf L]
static int cmd_transfer_send(const fs::path& data_dir, int argc, char** argv) {
    const auto to_s      = flag_val(argc, argv, "--to");
    const auto units_s   = flag_val(argc, argv, "--units");
    const auto origins_s = flag_all(argc, argv, "--origin");
    if (to_s.empty() || (units_s.empty() && origins_s.empty())) {
        std::cerr << "Usage: bc transfer send --to UID_HEX --units N "
                     "[--origin ISSUER_HEX:UNITS]... [--reason REF] [--via URL] [--leaf L]\n";
        return 1;
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);
    const std::string me_hex = to_hex(ctx.user_id.bytes);

    records::Transfer t{};
    t.from      = ctx.user_id.bytes;
    t.to        = uid_from_hex(to_s);
    t.timestamp = static_cast<int64_t>(std::time(nullptr));
    const auto reason_s = flag_val(argc, argv, "--reason");
    if (!reason_s.empty()) t.reason = parse_ref(reason_s);

    if (!origins_s.empty()) {
        // Manual portions.
        double total = 0;
        for (const auto& s : origins_s) {
            const auto colon = s.find(':');
            if (colon == std::string::npos)
                throw std::runtime_error("--origin expects ISSUER_HEX:UNITS, got: " + s);
            records::OriginQty o{};
            o.issuer = uid_from_hex(s.substr(0, colon));
            o.units  = std::stod(s.substr(colon + 1));
            if (o.units <= 0) throw std::runtime_error("--origin units must be positive");
            total += o.units;
            t.origins.push_back(o);
        }
        if (!units_s.empty() && std::abs(std::stod(units_s) - total) > 1e-9)
            throw std::runtime_error("--units disagrees with the sum of --origin portions");
    } else {
        t.origins = pick_portions(ctx, to_s, std::stod(units_s));
    }

    const Block block = append_record(ctx, Record{t});
    print_portions(t, me_hex, to_s);
    std::cout << "transfer ref: " << me_hex << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";

    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
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

    const Block block = fetch_block_from(via, src);
    if (block.type != BlockType::DATA)
        throw std::runtime_error("referenced block is not a DATA block");

    const auto rec = Codec::decode(block.payload.data(), block.payload.size());
    const auto* t  = std::get_if<records::Transfer>(&rec);
    if (!t) throw std::runtime_error("referenced record is not a Transfer");
    if (t->to != ctx.user_id.bytes)
        throw std::runtime_error("transfer is not addressed to this identity");
    if (t->from != block.address.user_id.bytes)
        throw std::runtime_error("transfer 'from' does not match the authoring chain");

    if (ctx.storage.has_external_block(block.address)) {
        std::cerr << "already received — wallet unchanged\n";
        return 0;
    }
    ctx.storage.put_external_block(block);
    append_record(ctx, Record{Copy{src}});   // on-chain acknowledgment (двусторонность)

    double total = 0;
    for (const auto& o : t->origins) total += o.units;
    std::cout << "received " << total << "h in " << t->origins.size()
              << " portion(s) from " << to_hex(t->from.data(), 32).substr(0, 16) << "...\n";
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

// bc pay --acceptance REF [--units N] [--via URL] [--leaf L]
// Pays (the remainder of) an own acceptance's appraisal to the worker
// (records.md §9.5). Refuses to exceed the appraised value — payments for one
// work never outgrow its labor_units (§12.8).
static int cmd_pay(const fs::path& data_dir, int argc, char** argv) {
    const auto acc_s = flag_val(argc, argv, "--acceptance");
    if (acc_s.empty()) {
        std::cerr << "Usage: bc pay --acceptance REF [--units N] [--via URL] [--leaf L]\n";
        return 1;
    }

    const NodeIndex leaf = parse_leaf_index(argc, argv);
    Context ctx(data_dir, leaf);

    const Ref acc_ref = parse_ref(acc_s);
    if (acc_ref.chain != ctx.user_id.bytes)
        throw std::runtime_error("--acceptance must reference your own record "
                                 "(the appraiser pays, records.md §9.5)");

    // One branch scan: locate the acceptance and total prior payments for it.
    std::optional<Acceptance> acceptance;
    double paid = 0;
    std::vector<Block> blocks;
    try { blocks = ctx.bc.get_branch(ctx.user_id, leaf); }
    catch (const BlockchainError&) {}
    for (const auto& b : blocks) {
        if (b.type != BlockType::DATA) continue;
        Record rec;
        try { rec = Codec::decode(b.payload.data(), b.payload.size()); }
        catch (const CodecError&) { continue; }
        if (Crypto::hash_block(b).bytes == acc_ref.hash) {
            const auto* a = std::get_if<Acceptance>(&rec);
            if (!a) throw std::runtime_error("--acceptance does not reference an Acceptance");
            acceptance = *a;
        } else if (const auto* t = std::get_if<records::Transfer>(&rec)) {
            if (t->reason && *t->reason == acc_ref)
                for (const auto& o : t->origins) paid += o.units;
        }
    }
    if (!acceptance)
        throw std::runtime_error("acceptance not found in the own branch");

    const double cap       = acceptance->labor_units;
    const double remaining = cap - paid;
    if (remaining <= 1e-9) {
        std::cerr << "already paid in full: " << paid << "/" << cap
                  << " labor-h — payments must not exceed the appraisal (§12.8)\n";
        return 1;
    }
    const auto   units_s = flag_val(argc, argv, "--units");
    const double units   = units_s.empty() ? remaining : std::stod(units_s);
    if (units > remaining + 1e-9) {
        std::cerr << "refused: " << units << "h would exceed the appraisal — paid "
                  << paid << "/" << cap << " labor-h, payable " << remaining
                  << "h (§12.8)\n";
        return 1;
    }

    const std::string me_hex = to_hex(ctx.user_id.bytes);
    const std::string to_s   = to_hex(acceptance->work.chain.data(), 32);

    records::Transfer t{};
    t.from      = ctx.user_id.bytes;
    t.to        = acceptance->work.chain;
    t.origins   = pick_portions(ctx, to_s, units);
    t.reason    = acc_ref;
    t.timestamp = static_cast<int64_t>(std::time(nullptr));

    const Block block = append_record(ctx, Record{t});
    print_portions(t, me_hex, to_s);
    std::cout << "paid " << units << "h (" << paid + units << "/" << cap
              << ")  transfer ref: " << me_hex << "/"
              << to_hex(Crypto::hash_block(block).bytes) << "\n";

    const auto via = flag_val(argc, argv, "--via");
    if (!via.empty()) upload_block(via, block);
    return 0;
}

// Shared GET for the aggregator economy view (records.md §13). Prints the
// JSON body as-is — every figure is re-checkable against the chains.
static int economy_get(const std::string& via, const std::string& path) {
    std::string host = via;
    if (host.rfind("http://", 0) == 0) host = host.substr(7);
    httplib::Client cli(host);
    cli.set_connection_timeout(5);
    const auto res = cli.Get(path);
    if (!res) {
        std::cerr << "aggregator unreachable: " << via << "\n";
        return 1;
    }
    std::cout << res->body << "\n";
    return res->status == 200 ? 0 : 1;
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
    std::cerr << "(settle with: bc transfer send --reason <pledge ref>; "
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
            if (!t->reason || t->reason->chain != ctx.user_id.bytes) continue;
            auto it = pledges.find(to_hex(t->reason->hash.data(), 32));
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
        std::string host = via;
        if (host.rfind("http://", 0) == 0) host = host.substr(7);
        httplib::Client cli(host);
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
        std::string host = via;
        if (host.rfind("http://", 0) == 0) host = host.substr(7);
        httplib::Client cli(host);
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

static void print_usage() {
    std::cerr <<
R"(Usage: bc [--data-dir PATH] <command> [--leaf INDEX]

Identity:
  identity create                  Generate keys and create default branch
  identity show                    Show your User ID

Branches:
  branch init <leaf_index>         Init a new branch (decimal or 0x hex)
  block stub                       Append an empty stub block (bootstrap merge / time anchor)

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
  specialty add <name>             Add a specialty
  grade add <spec_ref> <level>     Add a grade (level 1-6)
  work log                         Log a work event
    --agent  GRADE_REF
    --action "description"
    --hours  FLOAT
    [--start UNIX_TS]
    [--input  NAME:QTY:UNIT ...]   (repeatable)
    [--output NAME:QTY:UNIT ...]   (repeatable)
  accept                           Appraise received work (records.md §9.5)
    --work        WORK_REF             (fetch it first: bc fetch <ref> --via URL)
    --quality     TEXT
    [--hours-raw FLOAT]                default: hours of the fetched WorkRecord
    [--coef FLOAT] [--labor-units F]   default appraisal: hours-raw * coef
  pay --acceptance REF             Pay the worker up to the appraisal (§12.8)
    [--units N] [--via URL]            default: the unpaid remainder
  fetch <chain>/<hash> --via URL   Fetch any foreign block for local reading

Merge over a relay (sync.md §4.1):
  merge run                        Initiate a merge and drive it to completion
    --peer UID_HEX                     partner's User ID
    --via  URL                         aggregator relay, e.g. http://host:8080
    [--depth N] [--timeout SEC]        declared depth (1) / give-up time (60)
  discover --via URL               Ranked merge-partner suggestions (sync.md §8)
  merge serve --via URL            Respond to incoming merge OFFERs in a loop
    [--depth N] [--timeout SEC]        SEC=0: run forever
    [--once]                           exit after the first completed merge

Merge, manual relay (§6.4 bilateral two-round protocol):
  merge prepare                    Step 1: print own tip + snapshot blobs (send to partner)
  merge create --peer-tip HEX      Step 2: verify tip, union snapshots, create draft
               --peer-snapshot HEX          print draft_hash [--depth N: declared depth]
  merge cosign --draft HASH        Step 3: co-sign partner's draft_hash, print co_signature
  merge finalize --co-sig HEX      Step 4: attach partner co_sig, complete merge block

Seals (§7; gossip sync.md §7.2):
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

Economy (records.md §11 — именные трудочасы, §12.7):
  wallet                           Holdings per issuer + own debt in circulation
  transfer send                    Move labor-hours; auto-picks portions:
    --to UID_HEX --units N             receiver's paper first (redemption), then
    [--origin ISSUER_HEX:UNITS]...     other held paper, self-issue for the rest
    [--reason REF] [--via URL]         (--via also uploads the block for the receiver)
  transfer recv <chain>/<hash>     Fetch an incoming transfer from an aggregator,
    --via URL                          store it and acknowledge on-chain (Copy)
  pledge add --target REF          Promise labor-hours for a work/idea
    --units N [--executor UID_HEX] [--expires UNIX_TS]
  pledge revoke --pledge REF       Revoke the unsettled remainder of an own pledge
  pledge list                      Own pledges with settlement status
  ideas top --via URL              Funding board: pledged labor per idea (JSON)
  chain info UID_HEX --via URL     Economic dossier of a chain (JSON)

Sync cache (sync.md §5; gossip §7.1):
  cache list                       List cached participant leaves and compositions
  cache publish --via URL          Push the local cache to an aggregator warehouse
  cache complete --via URL         Pull everything missing under a snapshot root
                 [--root HEX]          (default: own branch's current snapshot)

Other:
  list                             List all records in a branch

--leaf INDEX  Target branch leaf index (decimal or 0x hex).
              Default: 0x7FFFFFFF. Valid range: 0x7FFFFFFF..0xFFFFFFFE.
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
        else if (cmd == "specialty" && subcmd == "add")     return cmd_specialty_add(data_dir, argc, argv);
        else if (cmd == "grade"     && subcmd == "add")     return cmd_grade_add(data_dir, argc, argv);
        else if (cmd == "work"      && subcmd == "log")     return cmd_work_log(data_dir, argc, argv);
        else if (cmd == "accept")                           return cmd_accept(data_dir, argc, argv);
        else if (cmd == "branch"    && subcmd == "init")    return cmd_branch_init(data_dir, argc, argv);
        else if (cmd == "block"     && subcmd == "stub")    return cmd_block_stub(data_dir, argc, argv);
        else if (cmd == "list")                             return cmd_list(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "prepare") return cmd_merge_prepare(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "create")  return cmd_merge_create(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "cosign")  return cmd_merge_cosign(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "finalize")return cmd_merge_finalize(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "run")     return cmd_merge_run(data_dir, argc, argv);
        else if (cmd == "merge"     && subcmd == "serve")   return cmd_merge_serve(data_dir, argc, argv);
        else if (cmd == "seal"      && subcmd == "add")     return cmd_seal_add(data_dir, argc, argv);
        else if (cmd == "seal"      && subcmd == "list")    return cmd_seal_list(data_dir, argc, argv);
        else if (cmd == "fraud"     && subcmd == "verify")  return cmd_fraud_verify(argc, argv);
        else if (cmd == "fraud"     && subcmd == "claim")   return cmd_fraud_claim(data_dir, argc, argv);
        else if (cmd == "cache"     && subcmd == "list")    return cmd_cache_list(data_dir);
        else if (cmd == "cache"     && subcmd == "publish") return cmd_cache_publish(data_dir, argc, argv);
        else if (cmd == "cache"     && subcmd == "complete")return cmd_cache_complete(data_dir, argc, argv);
        else if (cmd == "wallet")                           return cmd_wallet(data_dir, argc, argv);
        else if (cmd == "ideas"     && subcmd == "top")     return cmd_ideas_top(argc, argv);
        else if (cmd == "discover")                         return cmd_discover(data_dir, argc, argv);
        else if (cmd == "chain"     && subcmd == "info")    return cmd_chain_info(argc, argv);
        else if (cmd == "fetch")                            return cmd_fetch(data_dir, argc, argv);
        else if (cmd == "pay")                              return cmd_pay(data_dir, argc, argv);
        else if (cmd == "transfer"  && subcmd == "send")    return cmd_transfer_send(data_dir, argc, argv);
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
