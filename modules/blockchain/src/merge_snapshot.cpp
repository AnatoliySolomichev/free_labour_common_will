#include "blockchain/merge_snapshot.h"
#include "blockchain/merkle.h"

namespace blockchain {

MergeSnapshot MergeSnapshot::leaf(const ExternalRef& ref) {
    MergeSnapshot s;
    s.merkle_root = MerkleTree::leaf_hash(ref);
    s.hll.add(ref.address.user_id);
    return s;
}

MergeSnapshot MergeSnapshot::merge(const MergeSnapshot& a, const MergeSnapshot& b) {
    MergeSnapshot out;
    // Canonical child order (smaller root = left) makes the union commutative.
    out.merkle_root = (a.merkle_root.bytes <= b.merkle_root.bytes)
        ? MerkleTree::combine(a.merkle_root, b.merkle_root)
        : MerkleTree::combine(b.merkle_root, a.merkle_root);
    out.hll = a.hll;
    out.hll.merge(b.hll);
    return out;
}

} // namespace blockchain
