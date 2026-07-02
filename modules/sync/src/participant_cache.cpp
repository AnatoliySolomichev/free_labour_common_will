#include "sync/participant_cache.h"

#include <blockchain/serializer.h>

namespace chainsync {

using blockchain::Hash;
using blockchain::MerkleTree;

Hash ParticipantCache::put_leaf(const LeafRecord& record) {
    Hash key = MerkleTree::leaf_hash(record.ref);
    leaves_[key] = record;
    return key;
}

Hash ParticipantCache::put_composition(const Hash& a, const Hash& b) {
    // Canonical child order (smaller root = left), as in MergeSnapshot::merge.
    const Hash& left  = (a.bytes <= b.bytes) ? a : b;
    const Hash& right = (a.bytes <= b.bytes) ? b : a;
    Hash parent = MerkleTree::combine(left, right);
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
    return blockchain::Serializer::encode(*proof);
}

} // namespace chainsync
