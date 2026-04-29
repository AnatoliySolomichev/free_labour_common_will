#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/storage.h>
#include <blockchain/validator.h>
#include <records/codec.h>
#include <records/types.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace blockchain;
using namespace records;

// ── Constants ─────────────────────────────────────────────────────────────────

// First leaf (depth 32): all user writes go to this branch by default.
static constexpr NodeIndex DEFAULT_LEAF = 0xFFFF'FFFFu;   // 2^32 − 1

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
    "--data-dir", "--tag", "--kind", "--part",
    "--value", "--chain",
    "--agent", "--action", "--hours", "--start", "--input", "--output",
    "--work", "--quality", "--hours-raw", "--labor-units",
};

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

    explicit Context(const fs::path& data_dir)
        : storage  (data_dir / "db")
        , validator(storage)
        , bc       (storage, validator)
        , user_id  (load_user_id(data_dir))
        , working_kp(load_keypair(data_dir, DEFAULT_LEAF))
    {}
};

// ── Write helper: encode + append DATA block ──────────────────────────────────

static int cmd_write(const fs::path& data_dir, const Record& rec) {
    Context ctx(data_dir);
    const auto payload = Codec::encode(rec);
    const auto now     = static_cast<Timestamp>(std::time(nullptr));
    const auto block   = ctx.bc.append_data_block(
        ctx.user_id, DEFAULT_LEAF, payload, ctx.working_kp, now);
    const auto hash    = Crypto::hash_block(block);
    std::cout << "block #" << block.address.block_index
              << "  hash: " << to_hex(hash.bytes) << "\n";
    return 0;
}

// ── Commands ──────────────────────────────────────────────────────────────────

static int cmd_identity_create(const fs::path& data_dir) {
    if (fs::exists(key_path(data_dir, 0))) {
        std::cerr << "Identity already exists in " << data_dir
                  << " — run: bc identity show\n";
        return 1;
    }

    std::cerr << "Generating keys for 33 nodes (root → default branch)...\n";

    const auto path_idxs = path_indices(DEFAULT_LEAF);  // 33 indices: root to leaf
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

static int cmd_concept_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [concept, add, <text>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc concept add <text> [--tag TAG...]\n";
        return 1;
    }
    Concept c;
    c.text = pos[2];
    c.tags = flag_all(argc, argv, "--tag");
    return cmd_write(data_dir, c);
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
    return cmd_write(data_dir, cl);
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
    return cmd_write(data_dir, c);
}

static int cmd_copy(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [copy, <ref>]
    if (pos.size() < 2) {
        std::cerr << "Usage: bc copy <chain_hex>/<hash_hex>\n";
        return 1;
    }
    Copy c;
    c.source = parse_ref(pos[1]);
    return cmd_write(data_dir, c);
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
    return cmd_write(data_dir, r);
}

static int cmd_specialty_add(const fs::path& data_dir, int argc, char** argv) {
    const auto pos = get_positionals(argc, argv);  // [specialty, add, <name>]
    if (pos.size() < 3) {
        std::cerr << "Usage: bc specialty add <name>\n";
        return 1;
    }
    return cmd_write(data_dir, Specialty{ pos[2] });
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
    return cmd_write(data_dir, g);
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
    return cmd_write(data_dir, wr);
}

static int cmd_accept(const fs::path& data_dir, int argc, char** argv) {
    const auto work_s    = flag_val(argc, argv, "--work");
    const auto quality_s = flag_val(argc, argv, "--quality");
    const auto raw_s     = flag_val(argc, argv, "--hours-raw");
    const auto lu_s      = flag_val(argc, argv, "--labor-units");
    if (work_s.empty() || quality_s.empty() || raw_s.empty() || lu_s.empty()) {
        std::cerr << "Usage: bc accept\n"
                     "    --work         WORK_CHAIN/HASH\n"
                     "    --quality      TEXT\n"
                     "    --hours-raw    FLOAT\n"
                     "    --labor-units  FLOAT\n";
        return 1;
    }
    Acceptance a;
    a.work        = parse_ref(work_s);
    a.quality     = quality_s;
    a.hours_raw   = std::stod(raw_s);
    a.labor_units = std::stod(lu_s);
    a.timestamp   = static_cast<int64_t>(std::time(nullptr));
    a.receiver    = load_user_id(data_dir).bytes;
    return cmd_write(data_dir, a);
}

static int cmd_list(const fs::path& data_dir) {
    Context ctx(data_dir);

    std::vector<Block> blocks;
    try {
        blocks = ctx.bc.get_branch(ctx.user_id, DEFAULT_LEAF);
    } catch (const BlockchainError&) {
        // branch may be empty
    }

    if (blocks.empty()) {
        std::cout << "(no records yet — add one with: bc concept add \"text\")\n";
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

// ── Usage ─────────────────────────────────────────────────────────────────────

static void print_usage() {
    std::cerr <<
R"(Usage: bc [--data-dir PATH] <command>

Identity:
  identity create                  Generate keys and create default branch
  identity show                    Show your User ID

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
  accept                           Accept work → creates labor-hours
    --work        WORK_REF
    --quality     TEXT
    --hours-raw   FLOAT
    --labor-units FLOAT

Other:
  list                             List all records in default branch

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
        if      (cmd == "identity"  && subcmd == "create") return cmd_identity_create(data_dir);
        else if (cmd == "identity"  && subcmd == "show")   return cmd_identity_show(data_dir);
        else if (cmd == "concept"   && subcmd == "add")    return cmd_concept_add(data_dir, argc, argv);
        else if (cmd == "concept"   && subcmd == "link")   return cmd_concept_link(data_dir, argc, argv);
        else if (cmd == "composite" && subcmd == "add")    return cmd_composite_add(data_dir, argc, argv);
        else if (cmd == "copy")                            return cmd_copy(data_dir, argc, argv);
        else if (cmd == "react")                           return cmd_react(data_dir, argc, argv);
        else if (cmd == "specialty" && subcmd == "add")    return cmd_specialty_add(data_dir, argc, argv);
        else if (cmd == "grade"     && subcmd == "add")    return cmd_grade_add(data_dir, argc, argv);
        else if (cmd == "work"      && subcmd == "log")    return cmd_work_log(data_dir, argc, argv);
        else if (cmd == "accept")                          return cmd_accept(data_dir, argc, argv);
        else if (cmd == "list")                            return cmd_list(data_dir);
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
