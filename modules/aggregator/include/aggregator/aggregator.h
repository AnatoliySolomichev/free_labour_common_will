#pragma once
#include <atomic>

#include "blockchain/types.h"
#include "blockchain/crypto.h"
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace aggregator {

using namespace blockchain;

// ── Idea ─────────────────────────────────────────────────────────────────────
//
// An "idea" is a unique payload identified by its BLAKE2b-256 hash.
// Multiple users (witnesses) may have a block with byte-identical payload —
// they all reference the same idea.

struct IdeaInfo {
    Hash                     payload_hash;
    std::vector<uint8_t>     payload;
    std::vector<BlockAddress> witnesses;  // all blocks carrying this payload
};

// ── AggregatorStorage ─────────────────────────────────────────────────────────
//
// LMDB-backed store with five tables:
//   "blocks"      — block_key(40B) → CBOR(Block)
//   "hashes"      — block_hash(32B) → block_key(40B)   (for sync dedup)
//   "ideas"       — payload_hash(32B) → N×40B witness list
//   "snap_leaves" — leaf_hash(32B) → opaque LeafRecord bytes (sync.md §7.1)
//   "snap_comps"  — parent_root(32B) → left‖right (64B)
//
// The snapshot tables are a dumb warehouse: values are stored verbatim and
// never parsed — the *fetching* node verifies an entry against its own key
// (sync.md §7.1). First write wins: overwriting is refused, so a later
// attacker cannot displace an honest entry; a bogus first write is useless —
// clients discard it on verification.

class AggregatorStorage {
public:
    explicit AggregatorStorage(std::filesystem::path db_path);
    ~AggregatorStorage();

    AggregatorStorage(const AggregatorStorage&) = delete;
    AggregatorStorage& operator=(const AggregatorStorage&) = delete;

    // Add a block. Returns true if new, false if already present.
    // Throws: StorageError on LMDB failure.
    bool add_block(const Block& block);

    bool has_block_hash(const Hash& block_hash) const noexcept;
    bool has_block(const BlockAddress& address) const noexcept;

    // Returns nullopt if not found. By hash — a fork's rival too.
    std::optional<Block> get_block(const BlockAddress& address) const;
    std::optional<Block> get_block_by_hash(const Hash& block_hash) const;

    // Forks of a chain's branches (blockchain.md §6.7 rule 13): two blocks at one
    // address. The one that came first stays in place, the rival is kept by
    // its hash — add_block takes it and returns true. Readers decide which is
    // the owner's; an ancestor's revocation names the line.
    struct Fork {
        BlockAddress address;
        Hash         kept;    // the block in place
        Hash         rival;   // the one that came later
    };
    std::vector<Fork> forks(const UserId& chain) const;

    // The rival goes in place and the block that stood there goes aside
    // (blockchain.md §6.7 rules 9, 12): the caller found the rival the owner's and the other
    // not. Indexes follow — the hash, the payload index. False — no such rival.
    bool swap_fork(const Hash& rival);

    // All known block hashes (used to build sync manifest).
    std::vector<Hash> all_block_hashes() const;

    // All ideas (payload dedup index).
    std::vector<IdeaInfo> all_ideas() const;
    std::optional<IdeaInfo> get_idea(const Hash& payload_hash) const;

    std::size_t block_count() const noexcept;
    std::size_t idea_count() const noexcept;
    std::size_t revocation_count() const noexcept;

    // Unique per storage object in this process — with block_count() and
    // revocation_count() (both only ever grow) it names one state of what the
    // table of deals reads, so a reader may cache what it computed from it.
    // A swap of a fork (swap_fork) changes what stands without changing the
    // counts — it takes a fresh id, so the views recompute.
    uint64_t instance_id() const noexcept { return instance_id_.load(); }

    // Snapshot warehouse (sync.md §7.1). Values are opaque bytes.
    // put_* return false when the key already exists (first write wins).
    // Throws: StorageError on LMDB failure.
    bool put_snapshot_leaf(const Hash& leaf_hash, const std::vector<uint8_t>& bytes);
    bool put_snapshot_composition(const Hash& parent_root, const std::vector<uint8_t>& bytes);
    std::optional<std::vector<uint8_t>> get_snapshot_leaf(const Hash& leaf_hash) const;
    std::optional<std::vector<uint8_t>> get_snapshot_composition(const Hash& parent_root) const;
    std::vector<Hash> all_snapshot_leaf_hashes() const;
    std::vector<Hash> all_snapshot_composition_roots() const;

    // Revocation warehouse (sync.md §7.2; blockchain.md §6.7 rule 8).
    // Certificates are opaque bytes keyed chain(32)‖BLAKE2b(bytes)(32) —
    // identical certificates store once. The fetching node verifies them
    // autonomously (RevocationCert::verify) — the warehouse never does.
    // Throws: StorageError.
    bool put_revocation_bytes(const UserId& chain, const std::vector<uint8_t>& bytes);
    std::vector<std::vector<uint8_t>> get_revocation_bytes(const UserId& chain) const;
    std::vector<UserId> all_revoked_chains() const;

    // Key-path warehouse (sync.md §7.5; blockchain.md §7.5). Paths are opaque
    // bytes keyed signing_key(32)‖BLAKE2b(bytes)(32). The warehouse carries
    // them without looking inside: whether a path really leads from a chain's
    // root to this key is decided by the reader (KeyPathCert::verify), so a
    // forged claim costs the warehouse nothing and buys its author nothing.
    // Throws: StorageError.
    bool put_keypath_bytes(const PublicKey& key, const std::vector<uint8_t>& bytes);
    std::vector<std::vector<uint8_t>> get_keypath_bytes(const PublicKey& key) const;
    std::vector<PublicKey> all_keypath_keys() const;

    // When a block first came here (blockchain.md §9.4): it existed by then,
    // whatever time it claims. Recorded by add_block; nullopt for blocks stored
    // before arrivals were kept.
    std::optional<Timestamp> block_arrival(const Hash& block_hash) const;

    // Note that a block came here now — a revocation certificate's block, which
    // comes to the warehouse, not to add_block (blockchain.md §11.10). First write wins.
    void note_arrival(const Hash& block_hash);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<uint64_t> instance_id_;
};

} // namespace aggregator
