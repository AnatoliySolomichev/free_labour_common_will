#pragma once

#include "types.h"
#include "hll.h"

namespace blockchain {

// Accumulating participant snapshot of a merge packet (blockchain.md §6.5.1).
//
// Carries the two in-block commitments as the packet grows through the DAG:
//   - merkle_root: hierarchical root — parent = combine(child roots)
//   - hll:         unique-participant counter, merged by element-wise max
//
// A fresh participant is a single-leaf snapshot; merging two packets unions them.
// The empty-branch case (a participant with no block-0, hence no ExternalRef) is
// intentionally not handled here — its leaf representation is an open question.
struct MergeSnapshot {
    Hash      merkle_root;
    HllSketch hll;

    // Singleton snapshot for one participant contributing block `ref`.
    static MergeSnapshot leaf(const ExternalRef& ref);

    // Union of two packets: hierarchical Merkle combine + HLL merge.
    // Child order is canonicalised by root bytes, so merge is commutative:
    // both sides of a bilateral merge derive the same root.
    static MergeSnapshot merge(const MergeSnapshot& a, const MergeSnapshot& b);

    // hll_hash commitment placed into the MERGE block.
    Hash sketch_hash() const noexcept { return hll.sketch_hash(); }

    // Estimated number of unique participants covered by this snapshot.
    uint64_t estimate() const noexcept { return hll.estimate(); }

    bool operator==(const MergeSnapshot& o) const noexcept {
        return merkle_root == o.merkle_root && hll == o.hll;
    }
    bool operator!=(const MergeSnapshot& o) const noexcept { return !(*this == o); }
};

} // namespace blockchain
