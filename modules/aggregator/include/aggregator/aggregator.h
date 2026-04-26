#pragma once

#include "blockchain/types.h"
#include "blockchain/crypto.h"
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
// LMDB-backed store with three tables:
//   "blocks"  — block_key(40B) → CBOR(Block)
//   "hashes"  — block_hash(32B) → block_key(40B)   (for sync dedup)
//   "ideas"   — payload_hash(32B) → N×40B witness list

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

    // Returns nullopt if not found.
    std::optional<Block> get_block(const BlockAddress& address) const;
    std::optional<Block> get_block_by_hash(const Hash& block_hash) const;

    // All known block hashes (used to build sync manifest).
    std::vector<Hash> all_block_hashes() const;

    // All ideas (payload dedup index).
    std::vector<IdeaInfo> all_ideas() const;
    std::optional<IdeaInfo> get_idea(const Hash& payload_hash) const;

    std::size_t block_count() const noexcept;
    std::size_t idea_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aggregator
