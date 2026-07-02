#include "blockchain/storage.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <lmdb.h>
#include <cstring>

namespace blockchain {
namespace {

// ── LMDB error helper ─────────────────────────────────────────────────────────

void lcheck(int rc, const char* op) {
    if (rc != MDB_SUCCESS)
        throw StorageError(std::string(op) + ": " + mdb_strerror(rc));
}

// ── Key encoding (raw bytes, big-endian integers for correct LMDB sort order) ─

void be32(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >>  8);
    p[3] = static_cast<uint8_t>(v);
}

// Node key:  user_id (32 B) + node_index BE (4 B)  = 36 B
std::array<uint8_t, 36> node_key(const UserId& uid, NodeIndex ni) noexcept {
    std::array<uint8_t, 36> k;
    std::memcpy(k.data(), uid.bytes.data(), 32);
    be32(k.data() + 32, ni);
    return k;
}

// Block key: user_id (32 B) + node_index BE (4 B) + block_index BE (4 B) = 40 B
std::array<uint8_t, 40> block_key(const BlockAddress& addr) noexcept {
    std::array<uint8_t, 40> k;
    std::memcpy(k.data(), addr.user_id.bytes.data(), 32);
    be32(k.data() + 32, addr.node_index);
    be32(k.data() + 36, addr.block_index);
    return k;
}

// ── CBOR array wrapper for seal lists ─────────────────────────────────────────
//
// Seals are stored as:  array(N)[ bstr(seal_0_cbor), bstr(seal_1_cbor), ... ]
// Wrapping each seal in a bstr makes each element self-delimiting without
// needing to know the internal CBOR structure of a Seal.

void write_cbor_head(std::vector<uint8_t>& out, uint8_t major, uint64_t n) {
    const uint8_t base = static_cast<uint8_t>(major << 5);
    if (n <= 23)          { out.push_back(base | static_cast<uint8_t>(n)); }
    else if (n <= 0xFF)   { out.push_back(base | 24); out.push_back(static_cast<uint8_t>(n)); }
    else if (n <= 0xFFFF) {
        out.push_back(base | 25);
        out.push_back(static_cast<uint8_t>(n >> 8));
        out.push_back(static_cast<uint8_t>(n));
    } else {
        throw StorageError("seal list: count too large to encode");
    }
}

std::vector<uint8_t> encode_seal_list(const std::vector<Seal>& seals) {
    std::vector<uint8_t> out;
    write_cbor_head(out, 4, seals.size()); // array header
    for (const auto& s : seals) {
        auto enc = Serializer::encode(s);
        write_cbor_head(out, 2, enc.size()); // bstr header
        out.insert(out.end(), enc.begin(), enc.end());
    }
    return out;
}

std::vector<Seal> decode_seal_list(const uint8_t* data, size_t len) {
    if (len == 0) return {};
    size_t pos = 0;

    auto need = [&](size_t n) {
        if (pos + n > len) throw SerializationError("seal list: unexpected end");
    };
    auto read_head = [&](uint8_t expected_major) -> uint64_t {
        need(1);
        uint8_t b = data[pos++];
        if ((b >> 5) != expected_major)
            throw SerializationError("seal list: wrong CBOR major type");
        uint8_t info = b & 0x1fu;
        if (info <= 23) return info;
        if (info == 24) { need(1); return data[pos++]; }
        if (info == 25) {
            need(2);
            uint64_t v = (static_cast<uint64_t>(data[pos]) << 8) | data[pos+1];
            pos += 2; return v;
        }
        throw SerializationError("seal list: array/bstr count too large or indefinite");
    };

    uint64_t count = read_head(4); // major type 4 = array
    std::vector<Seal> seals;
    seals.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t elem_len = read_head(2); // major type 2 = bstr
        need(static_cast<size_t>(elem_len));
        seals.push_back(
            Serializer::decode_seal(data + pos, static_cast<size_t>(elem_len)));
        pos += static_cast<size_t>(elem_len);
    }
    return seals;
}

} // namespace (anonymous)

// ── LmdbStorage::Impl ─────────────────────────────────────────────────────────

struct LmdbStorage::Impl {
    MDB_env* env           = nullptr;
    MDB_dbi  dbi_nodes     = 0;
    MDB_dbi  dbi_blocks    = 0;
    MDB_dbi  dbi_seals     = 0;
    MDB_dbi  dbi_ext       = 0;
    MDB_dbi  dbi_snapshots = 0;
};

// ── Transaction ───────────────────────────────────────────────────────────────

namespace {
class LmdbTransaction : public IStorage::Transaction {
public:
    explicit LmdbTransaction(MDB_txn* txn) : txn_(txn), done_(false) {}
    ~LmdbTransaction() override { if (!done_) mdb_txn_abort(txn_); }
    void commit() override {
        lcheck(mdb_txn_commit(txn_), "transaction commit");
        done_ = true;
    }
private:
    MDB_txn* txn_;
    bool     done_;
};
} // namespace

// ── Constructor / Destructor ──────────────────────────────────────────────────

LmdbStorage::LmdbStorage(std::filesystem::path db_path)
    : impl_(std::make_unique<Impl>())
{
    std::filesystem::create_directories(db_path);

    lcheck(mdb_env_create(&impl_->env),             "mdb_env_create");
    lcheck(mdb_env_set_maxdbs(impl_->env, 5),       "mdb_env_set_maxdbs");
    lcheck(mdb_env_set_mapsize(impl_->env, 1u << 30),"mdb_env_set_mapsize"); // 1 GiB
    lcheck(mdb_env_open(impl_->env, db_path.c_str(), 0, 0600), "mdb_env_open");

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "open: txn_begin");
    int rc = 0;
    if ((rc = mdb_dbi_open(txn, "nodes",     MDB_CREATE, &impl_->dbi_nodes))     != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "blocks",    MDB_CREATE, &impl_->dbi_blocks))    != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "seals",     MDB_CREATE, &impl_->dbi_seals))     != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "external",  MDB_CREATE, &impl_->dbi_ext))       != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "snapshots", MDB_CREATE, &impl_->dbi_snapshots)) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        throw StorageError(std::string("mdb_dbi_open: ") + mdb_strerror(rc));
    }
    lcheck(mdb_txn_commit(txn), "open: txn_commit");
}

LmdbStorage::~LmdbStorage() {
    if (impl_ && impl_->env) {
        mdb_env_close(impl_->env);
        impl_->env = nullptr;
    }
}

LmdbStorage::LmdbStorage(LmdbStorage&&) noexcept = default;
LmdbStorage& LmdbStorage::operator=(LmdbStorage&&) noexcept = default;

// ── Nodes ─────────────────────────────────────────────────────────────────────

void LmdbStorage::put_node(const UserId& user_id, const Node& node) {
    auto k = node_key(user_id, node.index);
    auto v = Serializer::encode(node);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{v.size(), v.data()};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "put_node: begin");
    int rc = mdb_put(txn, impl_->dbi_nodes, &mk, &mv, MDB_NOOVERWRITE);
    if (rc == MDB_KEYEXIST) {
        mdb_txn_abort(txn);
        throw InvalidArgumentError(
            "put_node: node " + std::to_string(node.index) + " already exists");
    }
    if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "put_node: mdb_put"); }
    lcheck(mdb_txn_commit(txn), "put_node: commit");
}

Node LmdbStorage::get_node(const UserId& user_id, NodeIndex index) const {
    auto k = node_key(user_id, index);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_node: begin");
    int rc = mdb_get(txn, impl_->dbi_nodes, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); throw NodeNotFoundError(index); }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_node: mdb_get"); }
    try {
        Node n = Serializer::decode_node(
            static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        mdb_txn_abort(txn);
        return n;
    } catch (...) { mdb_txn_abort(txn); throw; }
}

bool LmdbStorage::has_node(const UserId& user_id, NodeIndex index) const noexcept {
    try {
        auto k = node_key(user_id, index);
        MDB_val mk{k.size(), k.data()};
        MDB_val mv{};
        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS)
            return false;
        int rc = mdb_get(txn, impl_->dbi_nodes, &mk, &mv);
        mdb_txn_abort(txn);
        return rc == MDB_SUCCESS;
    } catch (...) { return false; }
}

// ── Blocks ────────────────────────────────────────────────────────────────────

void LmdbStorage::put_block(const Block& block) {
    auto k = block_key(block.address);
    auto v = Serializer::encode(block);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{v.size(), v.data()};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "put_block: begin");
    int rc = mdb_put(txn, impl_->dbi_blocks, &mk, &mv, MDB_NOOVERWRITE);
    if (rc == MDB_KEYEXIST) {
        mdb_txn_abort(txn);
        throw InvalidArgumentError("put_block: duplicate block address");
    }
    if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "put_block: mdb_put"); }
    lcheck(mdb_txn_commit(txn), "put_block: commit");
}

Block LmdbStorage::get_block(const BlockAddress& address) const {
    auto k = block_key(address);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_block: begin");
    int rc = mdb_get(txn, impl_->dbi_blocks, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); throw BlockNotFoundError(address); }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_block: mdb_get"); }
    try {
        Block b = Serializer::decode_block(
            static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        mdb_txn_abort(txn);
        return b;
    } catch (...) { mdb_txn_abort(txn); throw; }
}

bool LmdbStorage::has_block(const BlockAddress& address) const noexcept {
    try {
        auto k = block_key(address);
        MDB_val mk{k.size(), k.data()};
        MDB_val mv{};
        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS)
            return false;
        int rc = mdb_get(txn, impl_->dbi_blocks, &mk, &mv);
        mdb_txn_abort(txn);
        return rc == MDB_SUCCESS;
    } catch (...) { return false; }
}

std::optional<BlockIndex> LmdbStorage::branch_tip_index(
    const UserId& user_id, NodeIndex leaf_index) const noexcept
{
    try {
        // Block keys are sorted as: user_id(32) | node_index_BE(4) | block_index_BE(4).
        // For a given (user_id, leaf_index) prefix, the last block has the highest
        // block_index.  We seek with the maximum possible block_index to land just
        // past the range, then step back one position.
        std::array<uint8_t, 36> prefix;
        std::memcpy(prefix.data(), user_id.bytes.data(), 32);
        be32(prefix.data() + 32, leaf_index);

        std::array<uint8_t, 40> seek_key;
        std::memcpy(seek_key.data(), prefix.data(), 36);
        seek_key[36] = seek_key[37] = seek_key[38] = seek_key[39] = 0xFF;

        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS)
            return std::nullopt;

        MDB_cursor* cur;
        if (mdb_cursor_open(txn, impl_->dbi_blocks, &cur) != MDB_SUCCESS) {
            mdb_txn_abort(txn); return std::nullopt;
        }

        MDB_val mk{seek_key.size(), seek_key.data()};
        MDB_val mv{};
        int rc = mdb_cursor_get(cur, &mk, &mv, MDB_SET_RANGE);
        if (rc == MDB_SUCCESS) {
            rc = mdb_cursor_get(cur, &mk, &mv, MDB_PREV); // step back from first key ≥ seek
        } else if (rc == MDB_NOTFOUND) {
            rc = mdb_cursor_get(cur, &mk, &mv, MDB_LAST); // seek past end → check last key
        }

        std::optional<BlockIndex> result;
        if (rc == MDB_SUCCESS && mk.mv_size == 40) {
            const auto* kd = static_cast<const uint8_t*>(mk.mv_data);
            if (std::memcmp(kd, prefix.data(), 36) == 0) {
                result = (static_cast<uint32_t>(kd[36]) << 24)
                       | (static_cast<uint32_t>(kd[37]) << 16)
                       | (static_cast<uint32_t>(kd[38]) <<  8)
                       |  static_cast<uint32_t>(kd[39]);
            }
        }

        mdb_cursor_close(cur);
        mdb_txn_abort(txn);
        return result;
    } catch (...) { return std::nullopt; }
}

// ── Seals ─────────────────────────────────────────────────────────────────────

void LmdbStorage::put_seal(const Seal& seal) {
    // Read + append + write-back in one write transaction for atomicity.
    std::array<uint8_t, 32> hash_buf = seal.block_hash.bytes;
    MDB_val mk{hash_buf.size(), hash_buf.data()};
    MDB_val mv{};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "put_seal: begin");

    std::vector<Seal> list;
    int rc = mdb_get(txn, impl_->dbi_seals, &mk, &mv);
    if (rc == MDB_SUCCESS) {
        try {
            list = decode_seal_list(
                static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        } catch (...) { mdb_txn_abort(txn); throw; }
    } else if (rc != MDB_NOTFOUND) {
        mdb_txn_abort(txn); lcheck(rc, "put_seal: mdb_get");
    }

    list.push_back(seal);
    auto encoded = encode_seal_list(list);
    MDB_val new_mv{encoded.size(), encoded.data()};

    rc = mdb_put(txn, impl_->dbi_seals, &mk, &new_mv, 0);
    if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "put_seal: mdb_put"); }
    lcheck(mdb_txn_commit(txn), "put_seal: commit");
}

std::vector<Seal> LmdbStorage::get_seals(const Hash& block_hash) const {
    std::array<uint8_t, 32> hash_buf = block_hash.bytes;
    MDB_val mk{hash_buf.size(), hash_buf.data()};
    MDB_val mv{};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_seals: begin");
    int rc = mdb_get(txn, impl_->dbi_seals, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); return {}; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_seals: mdb_get"); }
    try {
        auto seals = decode_seal_list(
            static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        mdb_txn_abort(txn);
        return seals;
    } catch (...) { mdb_txn_abort(txn); throw; }
}

// ── External blocks ───────────────────────────────────────────────────────────

void LmdbStorage::put_external_block(const Block& block) {
    auto k = block_key(block.address);
    auto v = Serializer::encode(block);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{v.size(), v.data()};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "put_external_block: begin");
    int rc = mdb_put(txn, impl_->dbi_ext, &mk, &mv, MDB_NOOVERWRITE);
    if (rc == MDB_KEYEXIST) {
        mdb_txn_abort(txn);
        throw InvalidArgumentError("put_external_block: duplicate");
    }
    if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "put_external_block: mdb_put"); }
    lcheck(mdb_txn_commit(txn), "put_external_block: commit");
}

Block LmdbStorage::get_external_block(const BlockAddress& address) const {
    auto k = block_key(address);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_external_block: begin");
    int rc = mdb_get(txn, impl_->dbi_ext, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); throw BlockNotFoundError(address); }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_external_block: mdb_get"); }
    try {
        Block b = Serializer::decode_block(
            static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        mdb_txn_abort(txn);
        return b;
    } catch (...) { mdb_txn_abort(txn); throw; }
}

bool LmdbStorage::has_external_block(const BlockAddress& address) const noexcept {
    try {
        auto k = block_key(address);
        MDB_val mk{k.size(), k.data()};
        MDB_val mv{};
        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS)
            return false;
        int rc = mdb_get(txn, impl_->dbi_ext, &mk, &mv);
        mdb_txn_abort(txn);
        return rc == MDB_SUCCESS;
    } catch (...) { return false; }
}

void LmdbStorage::for_each_external_block(
    std::function<bool(const Block&)> visitor) const
{
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn),
           "for_each_external_block: begin");

    MDB_cursor* cur;
    if (mdb_cursor_open(txn, impl_->dbi_ext, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        throw StorageError("for_each_external_block: cursor_open failed");
    }

    MDB_val mk{}, mv{};
    int rc = mdb_cursor_get(cur, &mk, &mv, MDB_FIRST);
    while (rc == MDB_SUCCESS) {
        try {
            Block b = Serializer::decode_block(
                static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
            if (!visitor(b)) break;
        } catch (const SerializationError&) {
            // skip malformed entries
        }
        rc = mdb_cursor_get(cur, &mk, &mv, MDB_NEXT);
    }

    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
}

// ── Merge snapshots ───────────────────────────────────────────────────────────

void LmdbStorage::put_snapshot(
    const UserId& user_id, NodeIndex leaf_index, const MergeSnapshot& snapshot)
{
    auto k = node_key(user_id, leaf_index);   // (user_id, leaf_index) — one per branch
    auto v = Serializer::encode(snapshot);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{v.size(), v.data()};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "put_snapshot: begin");
    // Overwrite allowed: a branch's snapshot grows with each merge.
    int rc = mdb_put(txn, impl_->dbi_snapshots, &mk, &mv, 0);
    if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "put_snapshot: mdb_put"); }
    lcheck(mdb_txn_commit(txn), "put_snapshot: commit");
}

std::optional<MergeSnapshot> LmdbStorage::get_snapshot(
    const UserId& user_id, NodeIndex leaf_index) const
{
    auto k = node_key(user_id, leaf_index);
    MDB_val mk{k.size(), k.data()};
    MDB_val mv{};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_snapshot: begin");
    int rc = mdb_get(txn, impl_->dbi_snapshots, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); return std::nullopt; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_snapshot: mdb_get"); }
    try {
        MergeSnapshot s = Serializer::decode_snapshot(
            static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        mdb_txn_abort(txn);
        return s;
    } catch (...) { mdb_txn_abort(txn); throw; }
}

// ── Transactions ──────────────────────────────────────────────────────────────

std::unique_ptr<IStorage::Transaction> LmdbStorage::begin_write() {
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "begin_write");
    return std::make_unique<LmdbTransaction>(txn);
}

} // namespace blockchain
