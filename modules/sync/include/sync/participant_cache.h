#pragma once

#include <blockchain/fraud.h>
#include <blockchain/merge_snapshot.h>
#include <blockchain/merkle.h>
#include <blockchain/types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// The sync module lives in namespace `chainsync`: plain `sync` collides with
// the POSIX ::sync() declared in <unistd.h>.
namespace chainsync {

// Raw participant-set cache (sync.md §5).
//
// A committed MergeSnapshot carries only the commitment (merkle_root + HLL),
// not the leaves themselves. This cache retains what the commitment omits —
// the leaves, node paths and evidence blocks — so that a FraudProofData
// (blockchain.md §6.5.6) can be assembled for any cached participant of any
// cached snapshot root.
//
// The cache is deliberately dumb storage: it does NOT validate node paths or
// evidence signatures on ingestion — FraudProof::verify is the judge. What it
// does guarantee is key consistency: leaf keys are recomputed from the record
// (leaf_hash(ref)) and composition parents are recomputed from the children
// (MerkleTree::combine), so an entry can never disagree with its own key.
//
// Filled by the merge orchestrator (§5.2), not by MergeSession — sync depends
// on blockchain, never the reverse. Eviction policy is an open question
// (§10.1); this first slice keeps everything in memory.

// Everything the snapshot commitment omits about one participant (§5.1).
struct LeafRecord {
    blockchain::ExternalRef       ref;        // (chain, node, block, committed hash)
    std::vector<blockchain::Node> node_path;  // root..ref.node of ref.chain
    blockchain::Block             evidence;   // participant's block-0 (preimage of block_hash)
};

// One hierarchical-merge step: parent_root = combine(left_child, right_child).
// Children are stored in canonical order (smaller root = left, §6.5.1).
struct Composition {
    blockchain::Hash left_child;
    blockchain::Hash right_child;
};

// LeafRecord wire/persistence codec: a FraudProofData with an empty Merkle
// path (reuses the blockchain Serializer). Shared by the persistent cache and
// the snapshot exchange (§7.1).
// Throws: SerializationError.
std::vector<uint8_t> encode_leaf_record(const LeafRecord& record);
LeafRecord decode_leaf_record(const uint8_t* data, std::size_t len);

class ParticipantCache {
public:
    // In-memory cache (tests, ephemeral use).
    ParticipantCache();

    // Persistent cache backed by LMDB at `dir` (created if missing): existing
    // entries are loaded eagerly, every put is written through. Everything is
    // retained — eviction policy is the open question §10.1.
    // Throws: StorageError, SerializationError (corrupt store).
    explicit ParticipantCache(const std::filesystem::path& dir);

    ~ParticipantCache();
    ParticipantCache(ParticipantCache&&) noexcept;
    ParticipantCache& operator=(ParticipantCache&&) noexcept;

    // Store `record` under leaf_hash(record.ref); overwrites an existing entry
    // with the same key. Returns the key.
    // Throws: SerializationError (from leaf_hash).
    blockchain::Hash put_leaf(const LeafRecord& record);

    // Record the merge of two snapshot roots. Order is canonicalised exactly as
    // in MergeSnapshot::merge and the parent is recomputed via
    // MerkleTree::combine, so put_composition(a, b) == put_composition(b, a)
    // and the stored triple always matches the committed root. Returns the
    // parent root.
    blockchain::Hash put_composition(const blockchain::Hash& a,
                                     const blockchain::Hash& b);

    std::optional<LeafRecord>  get_leaf(const blockchain::Hash& leaf_hash) const;
    std::optional<Composition> get_composition(const blockchain::Hash& parent_root) const;

    // Merkle inclusion path from target_root down to leaf_hash (§5.3): walks
    // the composition table, collecting siblings bottom-up. target_root ==
    // leaf_hash yields an empty proof (single-leaf snapshot). nullopt when no
    // composition chain connects the two — the leaf is not in that snapshot.
    // Purely structural: does not require the leaf record to be cached.
    std::optional<blockchain::MerkleTree::Proof> merkle_path(
        const blockchain::Hash& target_root,
        const blockchain::Hash& leaf_hash) const;

    // Assemble the full FraudProofData for a cached participant (§5.3):
    // merkle_path(target_root, leaf_hash) + the cached LeafRecord. nullopt if
    // either piece is missing. By construction the result satisfies
    // MerkleTree::verify(leaf_hash, .merkle_path, target_root).
    std::optional<blockchain::FraudProofData> build_proof(
        const blockchain::Hash& target_root,
        const blockchain::Hash& leaf_hash) const;

    // build_proof serialized with Serializer::encode — the opaque `proof`
    // bytes of a records::FraudClaim, checked by FraudProof::verify.
    // Throws: SerializationError.
    std::optional<std::vector<uint8_t>> build_proof_bytes(
        const blockchain::Hash& target_root,
        const blockchain::Hash& leaf_hash) const;

    std::size_t leaf_count()        const noexcept { return leaves_.size(); }
    std::size_t composition_count() const noexcept { return compositions_.size(); }

    // Full snapshots of the tables (CLI listing); order unspecified.
    std::vector<std::pair<blockchain::Hash, LeafRecord>>  leaves() const;
    std::vector<std::pair<blockchain::Hash, Composition>> compositions() const;

private:
    struct Persistence;   // LMDB write-through backend; null → in-memory only
    std::unique_ptr<Persistence> persist_;

    // Hash is uniform crypto output — its first bytes are already a good key.
    struct HashKey {
        std::size_t operator()(const blockchain::Hash& h) const noexcept {
            std::size_t v;
            std::memcpy(&v, h.bytes.data(), sizeof(v));
            return v;
        }
    };

    std::unordered_map<blockchain::Hash, LeafRecord, HashKey>  leaves_;
    std::unordered_map<blockchain::Hash, Composition, HashKey> compositions_;
};

// Feed one completed bilateral merge into the cache (§5.2): both single-leaf
// records — when a side merges for the first time its snapshot is still that
// leaf and the exchanged tip carries everything; a composite snapshot does not
// reveal its leaves (they arrive via gossip, §7) — plus the composition
// own_root × partner_root. Snapshots must be the PRE-merge ones the sides
// exchanged. Returns the union root (== the committed merkle_root).
// Throws: SerializationError, StorageError (persistent cache).
blockchain::Hash record_merge(ParticipantCache&                cache,
                              const blockchain::BranchTipInfo& own_tip,
                              const blockchain::MergeSnapshot& own_snapshot,
                              const blockchain::BranchTipInfo& partner_tip,
                              const blockchain::MergeSnapshot& partner_snapshot);

} // namespace chainsync
