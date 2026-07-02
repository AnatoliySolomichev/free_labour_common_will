#include "sync/participant_cache.h"

#include <blockchain/errors.h>
#include <blockchain/serializer.h>

#include <lmdb.h>

namespace chainsync {

using blockchain::BranchTipInfo;
using blockchain::ExternalRef;
using blockchain::FraudProofData;
using blockchain::Hash;
using blockchain::MergeSnapshot;
using blockchain::MerkleTree;
using blockchain::Serializer;
using blockchain::StorageError;

// ── LMDB persistence ──────────────────────────────────────────────────────────
//
// Two tables mirroring the in-memory maps. Values reuse existing codecs:
// a LeafRecord is stored as a FraudProofData with an empty Merkle path
// (identical fields otherwise); a Composition is the raw 64 bytes left‖right.

namespace {

void lcheck(int rc, const char* op) {
    if (rc != MDB_SUCCESS)
        throw StorageError(std::string(op) + ": " + mdb_strerror(rc));
}

std::vector<uint8_t> encode_leaf_record(const LeafRecord& r) {
    return Serializer::encode(
        FraudProofData{r.ref, MerkleTree::Proof{}, r.node_path, r.evidence});
}

LeafRecord decode_leaf_record(const uint8_t* data, size_t len) {
    FraudProofData p = Serializer::decode_fraud_proof(data, len);
    return LeafRecord{p.leaf, std::move(p.node_path), std::move(p.evidence)};
}

} // namespace (anonymous)

struct ParticipantCache::Persistence {
    MDB_env* env        = nullptr;
    MDB_dbi  dbi_leaves = 0;
    MDB_dbi  dbi_comps  = 0;

    explicit Persistence(const std::filesystem::path& dir) {
        std::filesystem::create_directories(dir);
        lcheck(mdb_env_create(&env),               "cache: mdb_env_create");
        lcheck(mdb_env_set_maxdbs(env, 2),         "cache: mdb_env_set_maxdbs");
        lcheck(mdb_env_set_mapsize(env, 1u << 30), "cache: mdb_env_set_mapsize"); // 1 GiB
        lcheck(mdb_env_open(env, dir.c_str(), 0, 0600), "cache: mdb_env_open");

        MDB_txn* txn;
        lcheck(mdb_txn_begin(env, nullptr, 0, &txn), "cache open: txn_begin");
        int rc;
        if ((rc = mdb_dbi_open(txn, "leaves",       MDB_CREATE, &dbi_leaves)) != MDB_SUCCESS ||
            (rc = mdb_dbi_open(txn, "compositions", MDB_CREATE, &dbi_comps))  != MDB_SUCCESS) {
            mdb_txn_abort(txn);
            throw StorageError(std::string("cache: mdb_dbi_open: ") + mdb_strerror(rc));
        }
        lcheck(mdb_txn_commit(txn), "cache open: txn_commit");
    }

    ~Persistence() {
        if (env) mdb_env_close(env);
    }

    Persistence(const Persistence&)            = delete;
    Persistence& operator=(const Persistence&) = delete;

    void put(MDB_dbi dbi, const Hash& key, const uint8_t* val, size_t len) {
        MDB_val mk{key.bytes.size(), const_cast<uint8_t*>(key.bytes.data())};
        MDB_val mv{len, const_cast<uint8_t*>(val)};
        MDB_txn* txn;
        lcheck(mdb_txn_begin(env, nullptr, 0, &txn), "cache put: txn_begin");
        int rc = mdb_put(txn, dbi, &mk, &mv, 0);
        if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "cache put: mdb_put"); }
        lcheck(mdb_txn_commit(txn), "cache put: txn_commit");
    }

    template <typename Fn>   // fn(key_ptr, key_len, val_ptr, val_len)
    void scan(MDB_dbi dbi, Fn&& fn) const {
        MDB_txn* txn;
        lcheck(mdb_txn_begin(env, nullptr, MDB_RDONLY, &txn), "cache scan: txn_begin");
        MDB_cursor* cur = nullptr;
        int rc = mdb_cursor_open(txn, dbi, &cur);
        if (rc != MDB_SUCCESS) { mdb_txn_abort(txn); lcheck(rc, "cache scan: cursor_open"); }
        MDB_val k, v;
        try {
            while ((rc = mdb_cursor_get(cur, &k, &v, MDB_NEXT)) == MDB_SUCCESS)
                fn(static_cast<const uint8_t*>(k.mv_data), k.mv_size,
                   static_cast<const uint8_t*>(v.mv_data), v.mv_size);
        } catch (...) {
            mdb_cursor_close(cur);
            mdb_txn_abort(txn);
            throw;
        }
        mdb_cursor_close(cur);
        mdb_txn_abort(txn);   // read-only
        if (rc != MDB_NOTFOUND) lcheck(rc, "cache scan: cursor_get");
    }
};

// ── Construction ──────────────────────────────────────────────────────────────

ParticipantCache::ParticipantCache() = default;

ParticipantCache::ParticipantCache(const std::filesystem::path& dir)
    : persist_(std::make_unique<Persistence>(dir)) {
    persist_->scan(persist_->dbi_leaves,
                   [this](const uint8_t* k, size_t kn, const uint8_t* v, size_t vn) {
        if (kn != sizeof(Hash{}.bytes))
            throw StorageError("cache: bad leaf key size");
        Hash key;
        std::memcpy(key.bytes.data(), k, key.bytes.size());
        leaves_[key] = decode_leaf_record(v, vn);
    });
    persist_->scan(persist_->dbi_comps,
                   [this](const uint8_t* k, size_t kn, const uint8_t* v, size_t vn) {
        if (kn != sizeof(Hash{}.bytes) || vn != 2 * sizeof(Hash{}.bytes))
            throw StorageError("cache: bad composition entry size");
        Hash key, left, right;
        std::memcpy(key.bytes.data(),   k,      key.bytes.size());
        std::memcpy(left.bytes.data(),  v,      left.bytes.size());
        std::memcpy(right.bytes.data(), v + 32, right.bytes.size());
        compositions_[key] = Composition{left, right};
    });
}

ParticipantCache::~ParticipantCache()                                   = default;
ParticipantCache::ParticipantCache(ParticipantCache&&) noexcept        = default;
ParticipantCache& ParticipantCache::operator=(ParticipantCache&&) noexcept = default;

// ── Puts ──────────────────────────────────────────────────────────────────────

Hash ParticipantCache::put_leaf(const LeafRecord& record) {
    Hash key = MerkleTree::leaf_hash(record.ref);
    if (persist_) {
        const auto bytes = encode_leaf_record(record);
        persist_->put(persist_->dbi_leaves, key, bytes.data(), bytes.size());
    }
    leaves_[key] = record;
    return key;
}

Hash ParticipantCache::put_composition(const Hash& a, const Hash& b) {
    // Canonical child order (smaller root = left), as in MergeSnapshot::merge.
    const Hash& left  = (a.bytes <= b.bytes) ? a : b;
    const Hash& right = (a.bytes <= b.bytes) ? b : a;
    Hash parent = MerkleTree::combine(left, right);
    if (persist_) {
        uint8_t buf[64];
        std::memcpy(buf,      left.bytes.data(),  32);
        std::memcpy(buf + 32, right.bytes.data(), 32);
        persist_->put(persist_->dbi_comps, parent, buf, sizeof(buf));
    }
    compositions_[parent] = Composition{left, right};
    return parent;
}

std::optional<LeafRecord>
ParticipantCache::get_leaf(const Hash& leaf_hash) const {
    auto it = leaves_.find(leaf_hash);
    if (it == leaves_.end()) return std::nullopt;
    return it->second;
}

std::optional<Composition>
ParticipantCache::get_composition(const Hash& parent_root) const {
    auto it = compositions_.find(parent_root);
    if (it == compositions_.end()) return std::nullopt;
    return it->second;
}

std::optional<MerkleTree::Proof>
ParticipantCache::merkle_path(const Hash& target_root,
                              const Hash& leaf_hash) const {
    if (target_root == leaf_hash) return MerkleTree::Proof{};

    // Iterative DFS from target_root down the composition DAG. `parent_of`
    // doubles as the visited set: shared subtrees (merge diamonds) are
    // explored once, and even an adversarial table cannot loop or blow the
    // stack — work is bounded by the number of compositions.
    std::unordered_map<Hash, Hash, HashKey> parent_of;
    std::vector<Hash> stack{target_root};
    bool found = false;
    while (!stack.empty()) {
        Hash cur = stack.back();
        stack.pop_back();
        if (cur == leaf_hash) { found = true; break; }
        auto it = compositions_.find(cur);
        if (it == compositions_.end()) continue;
        for (const Hash& child : {it->second.left_child, it->second.right_child}) {
            if (parent_of.emplace(child, cur).second)
                stack.push_back(child);
        }
    }
    if (!found) return std::nullopt;

    // Walk back up leaf → target_root; each step contributes the sibling.
    // Ascending order of collection == bottom-up order of MerkleTree::Proof.
    MerkleTree::Proof proof;
    Hash cur = leaf_hash;
    while (cur != target_root) {
        const Hash& parent = parent_of.at(cur);
        const Composition& comp = compositions_.at(parent);
        if (cur == comp.left_child) {
            proof.path.push_back(comp.right_child);
            proof.sibling_is_right.push_back(true);
        } else {
            proof.path.push_back(comp.left_child);
            proof.sibling_is_right.push_back(false);
        }
        cur = parent;
    }
    return proof;
}

std::optional<blockchain::FraudProofData>
ParticipantCache::build_proof(const Hash& target_root,
                              const Hash& leaf_hash) const {
    auto leaf = get_leaf(leaf_hash);
    if (!leaf) return std::nullopt;
    auto path = merkle_path(target_root, leaf_hash);
    if (!path) return std::nullopt;
    return blockchain::FraudProofData{
        leaf->ref, std::move(*path), std::move(leaf->node_path),
        std::move(leaf->evidence)};
}

std::optional<std::vector<uint8_t>>
ParticipantCache::build_proof_bytes(const Hash& target_root,
                                    const Hash& leaf_hash) const {
    auto proof = build_proof(target_root, leaf_hash);
    if (!proof) return std::nullopt;
    return Serializer::encode(*proof);
}

std::vector<std::pair<Hash, LeafRecord>> ParticipantCache::leaves() const {
    return {leaves_.begin(), leaves_.end()};
}

std::vector<std::pair<Hash, Composition>> ParticipantCache::compositions() const {
    return {compositions_.begin(), compositions_.end()};
}

// ── §5.2 fill rule ────────────────────────────────────────────────────────────

Hash record_merge(ParticipantCache&    cache,
                  const BranchTipInfo& own_tip,
                  const MergeSnapshot& own_snapshot,
                  const BranchTipInfo& partner_tip,
                  const MergeSnapshot& partner_snapshot) {
    const auto put_leaf_if_fresh = [&cache](const BranchTipInfo& tip,
                                            const MergeSnapshot& snapshot) {
        if (!tip.tip_block.has_value()) return;
        const ExternalRef ref{tip.tip_address, tip.tip_hash};
        if (MerkleTree::leaf_hash(ref) == snapshot.merkle_root)
            cache.put_leaf(LeafRecord{ref, tip.path, *tip.tip_block});
    };
    put_leaf_if_fresh(own_tip, own_snapshot);
    put_leaf_if_fresh(partner_tip, partner_snapshot);
    return cache.put_composition(own_snapshot.merkle_root,
                                 partner_snapshot.merkle_root);
}

} // namespace chainsync
