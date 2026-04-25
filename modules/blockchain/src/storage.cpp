#include "blockchain/storage.h"
#include "blockchain/errors.h"
#include <lmdb.h>

// TODO: implement LmdbStorage using LMDB.
// Table layout (see blockchain-api.md §9.2):
//   nodes          : (user_id, node_index) CBOR → Node CBOR
//   blocks         : (user_id, node_index, block_index) CBOR → Block CBOR
//   seals          : block_hash (32 bytes) → [Seal] CBOR
//   external_blocks: (user_id, node_index, block_index) CBOR → Block CBOR

namespace blockchain {

// ── Internal transaction stub ─────────────────────────────────────────────────

namespace {

class LmdbTransaction : public IStorage::Transaction {
public:
    void commit() override {
        throw StorageError("LmdbTransaction::commit: not implemented");
    }
};

} // namespace

// ── Impl (PIMPL for lmdb.h isolation) ────────────────────────────────────────

struct LmdbStorage::Impl {
    MDB_env* env = nullptr;
};

// ── LmdbStorage ───────────────────────────────────────────────────────────────

LmdbStorage::LmdbStorage(std::filesystem::path /*db_path*/)
    : impl_(std::make_unique<Impl>()) {
    throw StorageError("LmdbStorage: not implemented");
}

LmdbStorage::~LmdbStorage() {
    if (impl_ && impl_->env) {
        mdb_env_close(impl_->env);
    }
}

LmdbStorage::LmdbStorage(LmdbStorage&&) noexcept = default;
LmdbStorage& LmdbStorage::operator=(LmdbStorage&&) noexcept = default;

void LmdbStorage::put_node(const UserId&, const Node&) {
    throw StorageError("LmdbStorage::put_node: not implemented");
}

Node LmdbStorage::get_node(const UserId&, NodeIndex index) const {
    throw NodeNotFoundError(index);
}

bool LmdbStorage::has_node(const UserId&, NodeIndex) const noexcept {
    return false;
}

void LmdbStorage::put_block(const Block&) {
    throw StorageError("LmdbStorage::put_block: not implemented");
}

Block LmdbStorage::get_block(const BlockAddress& address) const {
    throw BlockNotFoundError(address);
}

bool LmdbStorage::has_block(const BlockAddress&) const noexcept {
    return false;
}

std::optional<BlockIndex> LmdbStorage::branch_tip_index(
    const UserId&, NodeIndex) const noexcept {
    return std::nullopt;
}

void LmdbStorage::put_seal(const Seal&) {
    throw StorageError("LmdbStorage::put_seal: not implemented");
}

std::vector<Seal> LmdbStorage::get_seals(const Hash&) const {
    return {};
}

void LmdbStorage::put_external_block(const Block&) {
    throw StorageError("LmdbStorage::put_external_block: not implemented");
}

Block LmdbStorage::get_external_block(const BlockAddress& address) const {
    throw BlockNotFoundError(address);
}

bool LmdbStorage::has_external_block(const BlockAddress&) const noexcept {
    return false;
}

std::unique_ptr<IStorage::Transaction> LmdbStorage::begin_write() {
    return std::make_unique<LmdbTransaction>();
}

} // namespace blockchain
