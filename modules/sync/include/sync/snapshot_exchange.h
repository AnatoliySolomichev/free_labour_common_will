#pragma once

#include "sync/participant_cache.h"

#include <blockchain/types.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace chainsync {

// ── Snapshot exchange (sync.md §7.1) ──────────────────────────────────────────
//
// Publish/fetch access to a snapshot warehouse: leaves (LeafRecord codec) and
// compositions keyed by their hashes. The warehouse is untrusted — it stores
// opaque bytes and never validates. All verification happens on the fetching
// side (complete_cache below): an entry that does not hash to its own key is
// discarded. Implementations return false/nullopt on transport failure and
// never throw.

class ISnapshotStore {
public:
    virtual ~ISnapshotStore() = default;

    // Publishing is idempotent: the warehouse keeps the first write.
    virtual bool put_leaf(const blockchain::Hash& leaf_hash,
                          const LeafRecord& record) = 0;
    virtual bool put_composition(const blockchain::Hash& parent_root,
                                 const Composition& composition) = 0;

    virtual std::optional<LeafRecord>
    fetch_leaf(const blockchain::Hash& leaf_hash) = 0;

    virtual std::optional<Composition>
    fetch_composition(const blockchain::Hash& parent_root) = 0;
};

// Push the whole local cache to the warehouse (idempotent).
// Returns the number of entries the warehouse newly stored.
std::size_t publish_cache(ISnapshotStore& store, const ParticipantCache& cache);

// Pull everything missing under `root` into the cache: walks the composition
// DAG top-down (visited set handles diamonds), fetching unknown compositions
// and leaves from the warehouse. Fetched entries are verified against their
// keys — leaf_hash(ref) for leaves, combine(left, right) for compositions —
// and dropped on mismatch, so a poisoned warehouse cannot corrupt the cache.
// BLAKE2b preimage resistance bounds the walk: under a committed root only
// the genuine subtree can verify. Returns the number of entries added.
std::size_t complete_cache(ISnapshotStore& store, ParticipantCache& cache,
                           const blockchain::Hash& root);

// ── HTTP warehouse on the aggregator (sync.md §7.1) ───────────────────────────
//
//   POST/GET /snapshot/leaf/{hash}         opaque LeafRecord bytes
//   POST/GET /snapshot/composition/{hash}  left‖right (64 bytes)

class HttpSnapshotStore : public ISnapshotStore {
public:
    // base_url example: "http://127.0.0.1:8080"
    explicit HttpSnapshotStore(const std::string& base_url);
    ~HttpSnapshotStore() override;

    bool put_leaf(const blockchain::Hash& leaf_hash,
                  const LeafRecord& record) override;
    bool put_composition(const blockchain::Hash& parent_root,
                         const Composition& composition) override;
    std::optional<LeafRecord>
    fetch_leaf(const blockchain::Hash& leaf_hash) override;
    std::optional<Composition>
    fetch_composition(const blockchain::Hash& parent_root) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chainsync
