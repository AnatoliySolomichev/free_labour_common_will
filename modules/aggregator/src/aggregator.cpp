#include "aggregator/aggregator.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <lmdb.h>
#include <cstring>
#include <stdexcept>

namespace aggregator {
namespace {

// ── LMDB helpers ─────────────────────────────────────────────────────────────

void lcheck(int rc, const char* op) {
    if (rc != MDB_SUCCESS)
        throw StorageError(std::string(op) + ": " + mdb_strerror(rc));
}

void be32(uint8_t* p, uint32_t v) noexcept {
    p[0]=(v>>24)&0xFF; p[1]=(v>>16)&0xFF; p[2]=(v>>8)&0xFF; p[3]=v&0xFF;
}

// block_key: user_id(32) + node_index_BE(4) + block_index_BE(4) = 40 bytes
std::array<uint8_t,40> block_key(const BlockAddress& a) noexcept {
    std::array<uint8_t,40> k;
    std::memcpy(k.data(), a.user_id.bytes.data(), 32);
    be32(k.data()+32, a.node_index);
    be32(k.data()+36, a.block_index);
    return k;
}

BlockAddress block_key_to_address(const uint8_t* k) noexcept {
    BlockAddress a{};
    std::memcpy(a.user_id.bytes.data(), k, 32);
    a.node_index  = (uint32_t(k[32])<<24)|(uint32_t(k[33])<<16)
                  |(uint32_t(k[34])<<8)| uint32_t(k[35]);
    a.block_index = (uint32_t(k[36])<<24)|(uint32_t(k[37])<<16)
                  |(uint32_t(k[38])<<8)| uint32_t(k[39]);
    return a;
}

// Witness list encoding: N×40 raw bytes (one block_key per witness).
std::vector<uint8_t> encode_witnesses(const std::vector<BlockAddress>& ws) {
    std::vector<uint8_t> out(ws.size() * 40);
    for (std::size_t i = 0; i < ws.size(); ++i) {
        auto k = block_key(ws[i]);
        std::memcpy(out.data() + i*40, k.data(), 40);
    }
    return out;
}

std::vector<BlockAddress> decode_witnesses(const uint8_t* data, std::size_t len) {
    if (len % 40 != 0) throw SerializationError("witness list: length not multiple of 40");
    std::vector<BlockAddress> ws;
    ws.reserve(len / 40);
    for (std::size_t i = 0; i < len; i += 40)
        ws.push_back(block_key_to_address(data + i));
    return ws;
}

} // anonymous namespace

// ── Impl ──────────────────────────────────────────────────────────────────────

struct AggregatorStorage::Impl {
    MDB_env* env       = nullptr;
    MDB_dbi  dbi_blk   = 0; // blocks:      block_key  → CBOR(Block)
    MDB_dbi  dbi_hash  = 0; // hashes:      block_hash → block_key
    MDB_dbi  dbi_idea  = 0; // ideas:       payload_hash → N×40B witness list
    MDB_dbi  dbi_leaf  = 0; // snap_leaves: leaf_hash → opaque LeafRecord bytes
    MDB_dbi  dbi_comp  = 0; // snap_comps:  parent_root → left‖right (64B)
    MDB_dbi  dbi_seal  = 0; // seals: block_hash(32)‖blake2b(bytes)(32) → seal bytes
    MDB_dbi  dbi_rev   = 0; // revocations: chain(32)‖blake2b(bytes)(32) → certificate bytes
};

// ── Constructor / Destructor ──────────────────────────────────────────────────

AggregatorStorage::AggregatorStorage(std::filesystem::path db_path)
    : impl_(std::make_unique<Impl>())
{
    std::filesystem::create_directories(db_path);
    lcheck(mdb_env_create(&impl_->env),              "mdb_env_create");
    lcheck(mdb_env_set_maxdbs(impl_->env, 7),        "mdb_env_set_maxdbs");
    lcheck(mdb_env_set_mapsize(impl_->env, 1u<<30),  "mdb_env_set_mapsize");
    lcheck(mdb_env_open(impl_->env, db_path.c_str(), 0, 0600), "mdb_env_open");

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "open txn");
    int rc = 0;
    if ((rc = mdb_dbi_open(txn, "blocks",      MDB_CREATE, &impl_->dbi_blk))  != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "hashes",      MDB_CREATE, &impl_->dbi_hash)) != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "ideas",       MDB_CREATE, &impl_->dbi_idea)) != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "snap_leaves", MDB_CREATE, &impl_->dbi_leaf)) != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "snap_comps",  MDB_CREATE, &impl_->dbi_comp)) != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "seals",       MDB_CREATE, &impl_->dbi_seal)) != MDB_SUCCESS ||
        (rc = mdb_dbi_open(txn, "revocations", MDB_CREATE, &impl_->dbi_rev))  != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        throw StorageError(std::string("mdb_dbi_open: ") + mdb_strerror(rc));
    }
    lcheck(mdb_txn_commit(txn), "open commit");
}

AggregatorStorage::~AggregatorStorage() {
    if (impl_ && impl_->env) { mdb_env_close(impl_->env); impl_->env = nullptr; }
}

// ── add_block ─────────────────────────────────────────────────────────────────

bool AggregatorStorage::add_block(const Block& block) {
    Hash block_hash   = Crypto::hash_block(block);
    Hash payload_hash = Crypto::hash(block.payload.data(), block.payload.size());

    auto bk  = block_key(block.address);
    auto cbor = Serializer::encode(block);

    MDB_val mk_bk   {bk.size(),               bk.data()};
    MDB_val mk_bh   {block_hash.bytes.size(),  block_hash.bytes.data()};
    MDB_val mk_ph   {payload_hash.bytes.size(),payload_hash.bytes.data()};

    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "add_block: begin");

    // Check duplicate via hashes table.
    {
        MDB_val mv{};
        int rc = mdb_get(txn, impl_->dbi_hash, &mk_bh, &mv);
        if (rc == MDB_SUCCESS) { mdb_txn_abort(txn); return false; } // already known
        if (rc != MDB_NOTFOUND) { mdb_txn_abort(txn); lcheck(rc, "add_block: hash check"); }
    }

    // Write block.
    {
        MDB_val mv{cbor.size(), cbor.data()};
        int rc = mdb_put(txn, impl_->dbi_blk, &mk_bk, &mv, MDB_NOOVERWRITE);
        if (rc != MDB_SUCCESS && rc != MDB_KEYEXIST)
            { mdb_txn_abort(txn); lcheck(rc, "add_block: put block"); }
    }

    // Write hash→block_key mapping.
    {
        MDB_val mv{bk.size(), bk.data()};
        int rc = mdb_put(txn, impl_->dbi_hash, &mk_bh, &mv, 0);
        if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "add_block: put hash"); }
    }

    // Update ideas: read existing witness list, append new address.
    {
        MDB_val mv{};
        std::vector<BlockAddress> witnesses;
        int rc = mdb_get(txn, impl_->dbi_idea, &mk_ph, &mv);
        if (rc == MDB_SUCCESS)
            witnesses = decode_witnesses(static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        else if (rc != MDB_NOTFOUND)
            { mdb_txn_abort(txn); lcheck(rc, "add_block: get idea"); }

        witnesses.push_back(block.address);
        auto enc = encode_witnesses(witnesses);
        MDB_val new_mv{enc.size(), enc.data()};
        rc = mdb_put(txn, impl_->dbi_idea, &mk_ph, &new_mv, 0);
        if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "add_block: put idea"); }
    }

    lcheck(mdb_txn_commit(txn), "add_block: commit");
    return true;
}

// ── Queries ───────────────────────────────────────────────────────────────────

bool AggregatorStorage::has_block_hash(const Hash& bh) const noexcept {
    try {
        MDB_val mk{(bh.bytes.size()),
                   const_cast<uint8_t*>(bh.bytes.data())};
        MDB_val mv{};
        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return false;
        int rc = mdb_get(txn, impl_->dbi_hash, &mk, &mv);
        mdb_txn_abort(txn);
        return rc == MDB_SUCCESS;
    } catch (...) { return false; }
}

bool AggregatorStorage::has_block(const BlockAddress& addr) const noexcept {
    try {
        auto k  = block_key(addr);
        MDB_val mk{k.size(), k.data()}, mv{};
        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return false;
        int rc = mdb_get(txn, impl_->dbi_blk, &mk, &mv);
        mdb_txn_abort(txn);
        return rc == MDB_SUCCESS;
    } catch (...) { return false; }
}

std::optional<Block> AggregatorStorage::get_block(const BlockAddress& addr) const {
    auto k = block_key(addr);
    MDB_val mk{k.size(), k.data()}, mv{};
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_block: begin");
    int rc = mdb_get(txn, impl_->dbi_blk, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); return std::nullopt; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_block"); }
    try {
        Block b = Serializer::decode_block(static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        mdb_txn_abort(txn);
        return b;
    } catch (...) { mdb_txn_abort(txn); throw; }
}

std::optional<Block> AggregatorStorage::get_block_by_hash(const Hash& bh) const {
    MDB_val mk{bh.bytes.size(), const_cast<uint8_t*>(bh.bytes.data())}, mv{};
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_by_hash: begin");
    int rc = mdb_get(txn, impl_->dbi_hash, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); return std::nullopt; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_by_hash: hash lookup"); }
    BlockAddress addr = block_key_to_address(static_cast<const uint8_t*>(mv.mv_data));
    mdb_txn_abort(txn);
    return get_block(addr);
}

std::vector<Hash> AggregatorStorage::all_block_hashes() const {
    std::vector<Hash> result;
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "all_hashes: begin");
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, impl_->dbi_hash, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn); return {};
    }
    MDB_val mk{}, mv{};
    while (mdb_cursor_get(cur, &mk, &mv, MDB_NEXT) == MDB_SUCCESS) {
        if (mk.mv_size == 32) {
            Hash h;
            std::memcpy(h.bytes.data(), mk.mv_data, 32);
            result.push_back(h);
        }
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return result;
}


std::vector<IdeaInfo> AggregatorStorage::all_ideas() const {
    std::vector<IdeaInfo> result;
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "all_ideas: begin");
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, impl_->dbi_idea, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn); return {};
    }
    MDB_val mk{}, mv{};
    while (mdb_cursor_get(cur, &mk, &mv, MDB_NEXT) == MDB_SUCCESS) {
        if (mk.mv_size != 32) continue;
        IdeaInfo info{};
        std::memcpy(info.payload_hash.bytes.data(), mk.mv_data, 32);
        info.witnesses = decode_witnesses(
            static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
        result.push_back(std::move(info));
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    // Load payloads (separate read txns).
    for (auto& info : result) {
        if (!info.witnesses.empty()) {
            auto blk = get_block(info.witnesses[0]);
            if (blk) info.payload = blk->payload;
        }
    }
    return result;
}

std::optional<IdeaInfo> AggregatorStorage::get_idea(const Hash& ph) const {
    MDB_val mk{ph.bytes.size(), const_cast<uint8_t*>(ph.bytes.data())}, mv{};
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_idea: begin");
    int rc = mdb_get(txn, impl_->dbi_idea, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); return std::nullopt; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "get_idea"); }
    IdeaInfo info{};
    info.payload_hash = ph;
    info.witnesses = decode_witnesses(static_cast<const uint8_t*>(mv.mv_data), mv.mv_size);
    mdb_txn_abort(txn);
    if (!info.witnesses.empty()) {
        auto blk = get_block(info.witnesses[0]);
        if (blk) info.payload = blk->payload;
    }
    return info;
}

// ── Snapshot warehouse (sync.md §7.1) ─────────────────────────────────────────
//
// Generic 32-byte-key → opaque-bytes access shared by both tables.

namespace {

bool put_opaque(MDB_env* env, MDB_dbi dbi, const Hash& key,
                const std::vector<uint8_t>& bytes, const char* op) {
    MDB_val mk{key.bytes.size(), const_cast<uint8_t*>(key.bytes.data())};
    MDB_val mv{bytes.size(),     const_cast<uint8_t*>(bytes.data())};
    MDB_txn* txn;
    lcheck(mdb_txn_begin(env, nullptr, 0, &txn), op);
    int rc = mdb_put(txn, dbi, &mk, &mv, MDB_NOOVERWRITE);
    if (rc == MDB_KEYEXIST) { mdb_txn_abort(txn); return false; }   // first write wins
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, op); }
    lcheck(mdb_txn_commit(txn), op);
    return true;
}

std::optional<std::vector<uint8_t>> get_opaque(MDB_env* env, MDB_dbi dbi,
                                               const Hash& key, const char* op) {
    MDB_val mk{key.bytes.size(), const_cast<uint8_t*>(key.bytes.data())};
    MDB_val mv{};
    MDB_txn* txn;
    lcheck(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn), op);
    int rc = mdb_get(txn, dbi, &mk, &mv);
    if (rc == MDB_NOTFOUND) { mdb_txn_abort(txn); return std::nullopt; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, op); }
    std::vector<uint8_t> out(static_cast<const uint8_t*>(mv.mv_data),
                             static_cast<const uint8_t*>(mv.mv_data) + mv.mv_size);
    mdb_txn_abort(txn);
    return out;
}

std::vector<Hash> all_keys(MDB_env* env, MDB_dbi dbi, const char* op) {
    std::vector<Hash> result;
    MDB_txn* txn;
    lcheck(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn), op);
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, dbi, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn); return {};
    }
    MDB_val mk{}, mv{};
    while (mdb_cursor_get(cur, &mk, &mv, MDB_NEXT) == MDB_SUCCESS) {
        if (mk.mv_size == 32) {
            Hash h;
            std::memcpy(h.bytes.data(), mk.mv_data, 32);
            result.push_back(h);
        }
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return result;
}

} // anonymous namespace

bool AggregatorStorage::put_snapshot_leaf(const Hash& leaf_hash,
                                          const std::vector<uint8_t>& bytes) {
    return put_opaque(impl_->env, impl_->dbi_leaf, leaf_hash, bytes, "put_snap_leaf");
}

bool AggregatorStorage::put_snapshot_composition(const Hash& parent_root,
                                                 const std::vector<uint8_t>& bytes) {
    return put_opaque(impl_->env, impl_->dbi_comp, parent_root, bytes, "put_snap_comp");
}

std::optional<std::vector<uint8_t>>
AggregatorStorage::get_snapshot_leaf(const Hash& leaf_hash) const {
    return get_opaque(impl_->env, impl_->dbi_leaf, leaf_hash, "get_snap_leaf");
}

std::optional<std::vector<uint8_t>>
AggregatorStorage::get_snapshot_composition(const Hash& parent_root) const {
    return get_opaque(impl_->env, impl_->dbi_comp, parent_root, "get_snap_comp");
}

std::vector<Hash> AggregatorStorage::all_snapshot_leaf_hashes() const {
    return all_keys(impl_->env, impl_->dbi_leaf, "snap_leaf_manifest");
}

std::vector<Hash> AggregatorStorage::all_snapshot_composition_roots() const {
    return all_keys(impl_->env, impl_->dbi_comp, "snap_comp_manifest");
}

// ── Seal warehouse (sync.md §7.2) ─────────────────────────────────────────────

bool AggregatorStorage::put_seal_bytes(const Hash& block_hash,
                                       const std::vector<uint8_t>& bytes) {
    const Hash content = Crypto::hash(bytes.data(), bytes.size());
    std::array<uint8_t, 64> key{};
    std::memcpy(key.data(),      block_hash.bytes.data(), 32);
    std::memcpy(key.data() + 32, content.bytes.data(),    32);

    MDB_val mk{key.size(),   key.data()};
    MDB_val mv{bytes.size(), const_cast<uint8_t*>(bytes.data())};
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "put_seal: begin");
    int rc = mdb_put(txn, impl_->dbi_seal, &mk, &mv, MDB_NOOVERWRITE);
    if (rc == MDB_KEYEXIST) { mdb_txn_abort(txn); return false; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "put_seal"); }
    lcheck(mdb_txn_commit(txn), "put_seal: commit");
    return true;
}

std::vector<std::vector<uint8_t>>
AggregatorStorage::get_seal_bytes(const Hash& block_hash) const {
    std::vector<std::vector<uint8_t>> result;
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_seals: begin");
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, impl_->dbi_seal, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn); return {};
    }
    std::array<uint8_t, 64> prefix{};
    std::memcpy(prefix.data(), block_hash.bytes.data(), 32);
    MDB_val mk{prefix.size(), prefix.data()}, mv{};
    int rc = mdb_cursor_get(cur, &mk, &mv, MDB_SET_RANGE);
    while (rc == MDB_SUCCESS && mk.mv_size == 64 &&
           std::memcmp(mk.mv_data, block_hash.bytes.data(), 32) == 0) {
        result.emplace_back(static_cast<const uint8_t*>(mv.mv_data),
                            static_cast<const uint8_t*>(mv.mv_data) + mv.mv_size);
        rc = mdb_cursor_get(cur, &mk, &mv, MDB_NEXT);
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return result;
}

std::vector<Hash> AggregatorStorage::all_sealed_block_hashes() const {
    std::vector<Hash> result;
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "seal_manifest: begin");
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, impl_->dbi_seal, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn); return {};
    }
    MDB_val mk{}, mv{};
    while (mdb_cursor_get(cur, &mk, &mv, MDB_NEXT) == MDB_SUCCESS) {
        if (mk.mv_size != 64) continue;
        Hash h;
        std::memcpy(h.bytes.data(), mk.mv_data, 32);
        if (result.empty() || !(result.back() == h))   // keys sorted: dedupe run
            result.push_back(h);
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return result;
}

// ── Revocation warehouse (sync.md §7.2; blockchain.md §6.7 rule 8) ────────────

bool AggregatorStorage::put_revocation_bytes(const UserId& chain,
                                             const std::vector<uint8_t>& bytes) {
    const Hash content = Crypto::hash(bytes.data(), bytes.size());
    std::array<uint8_t, 64> key{};
    std::memcpy(key.data(),      chain.bytes.data(),   32);
    std::memcpy(key.data() + 32, content.bytes.data(), 32);

    MDB_val mk{key.size(),   key.data()};
    MDB_val mv{bytes.size(), const_cast<uint8_t*>(bytes.data())};
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, 0, &txn), "put_revocation: begin");
    int rc = mdb_put(txn, impl_->dbi_rev, &mk, &mv, MDB_NOOVERWRITE);
    if (rc == MDB_KEYEXIST) { mdb_txn_abort(txn); return false; }
    if (rc != MDB_SUCCESS)  { mdb_txn_abort(txn); lcheck(rc, "put_revocation"); }
    lcheck(mdb_txn_commit(txn), "put_revocation: commit");
    return true;
}

std::vector<std::vector<uint8_t>>
AggregatorStorage::get_revocation_bytes(const UserId& chain) const {
    std::vector<std::vector<uint8_t>> result;
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "get_revocations: begin");
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, impl_->dbi_rev, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn); return {};
    }
    std::array<uint8_t, 64> prefix{};
    std::memcpy(prefix.data(), chain.bytes.data(), 32);
    MDB_val mk{prefix.size(), prefix.data()}, mv{};
    int rc = mdb_cursor_get(cur, &mk, &mv, MDB_SET_RANGE);
    while (rc == MDB_SUCCESS && mk.mv_size == 64 &&
           std::memcmp(mk.mv_data, chain.bytes.data(), 32) == 0) {
        result.emplace_back(static_cast<const uint8_t*>(mv.mv_data),
                            static_cast<const uint8_t*>(mv.mv_data) + mv.mv_size);
        rc = mdb_cursor_get(cur, &mk, &mv, MDB_NEXT);
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return result;
}

std::vector<UserId> AggregatorStorage::all_revoked_chains() const {
    std::vector<UserId> result;
    MDB_txn* txn;
    lcheck(mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn), "rev_manifest: begin");
    MDB_cursor* cur;
    if (mdb_cursor_open(txn, impl_->dbi_rev, &cur) != MDB_SUCCESS) {
        mdb_txn_abort(txn); return {};
    }
    MDB_val mk{}, mv{};
    while (mdb_cursor_get(cur, &mk, &mv, MDB_NEXT) == MDB_SUCCESS) {
        if (mk.mv_size != 64) continue;
        UserId c;
        std::memcpy(c.bytes.data(), mk.mv_data, 32);
        if (result.empty() || !(result.back() == c))   // keys sorted: dedupe run
            result.push_back(c);
    }
    mdb_cursor_close(cur);
    mdb_txn_abort(txn);
    return result;
}

std::size_t AggregatorStorage::block_count() const noexcept {
    try {
        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return 0;
        MDB_stat st{};
        mdb_stat(txn, impl_->dbi_blk, &st);
        mdb_txn_abort(txn);
        return static_cast<std::size_t>(st.ms_entries);
    } catch (...) { return 0; }
}

std::size_t AggregatorStorage::idea_count() const noexcept {
    try {
        MDB_txn* txn;
        if (mdb_txn_begin(impl_->env, nullptr, MDB_RDONLY, &txn) != MDB_SUCCESS) return 0;
        MDB_stat st{};
        mdb_stat(txn, impl_->dbi_idea, &st);
        mdb_txn_abort(txn);
        return static_cast<std::size_t>(st.ms_entries);
    } catch (...) { return 0; }
}

} // namespace aggregator
