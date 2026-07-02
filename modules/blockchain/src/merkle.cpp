#include "blockchain/merkle.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace blockchain {
namespace {

// Internal node: 0x01 || left || right  (domain-separated from leaves).
Hash node_hash(const Hash& l, const Hash& r) noexcept {
    std::array<uint8_t, 1 + 32 + 32> buf{};
    buf[0] = 0x01;
    std::memcpy(buf.data() + 1,  l.bytes.data(), 32);
    std::memcpy(buf.data() + 33, r.bytes.data(), 32);
    return Crypto::hash(buf.data(), buf.size());
}

// One level up: pair adjacent nodes; promote a lone odd tail unchanged.
std::vector<Hash> next_level(const std::vector<Hash>& level) noexcept {
    std::vector<Hash> up;
    up.reserve((level.size() + 1) / 2);
    for (size_t i = 0; i + 1 < level.size(); i += 2)
        up.push_back(node_hash(level[i], level[i + 1]));
    if (level.size() % 2 == 1)
        up.push_back(level.back());   // promoted unchanged
    return up;
}

} // namespace (anonymous)

Hash MerkleTree::leaf_hash(const ExternalRef& ref) {
    std::vector<uint8_t> cbor = Serializer::encode(ref);
    std::vector<uint8_t> buf;
    buf.reserve(1 + cbor.size());
    buf.push_back(0x00);              // leaf domain tag
    buf.insert(buf.end(), cbor.begin(), cbor.end());
    return Crypto::hash(buf.data(), buf.size());
}

Hash MerkleTree::root(const std::vector<Hash>& leaves) noexcept {
    if (leaves.empty()) return Hash::zero();
    std::vector<Hash> level = leaves;
    while (level.size() > 1)
        level = next_level(level);
    return level.front();
}

Hash MerkleTree::combine(const Hash& left, const Hash& right) noexcept {
    return node_hash(left, right);
}

MerkleTree::Proof MerkleTree::make_proof(const std::vector<Hash>& leaves,
                                         size_t leaf_index) {
    if (leaf_index >= leaves.size())
        throw InvalidArgumentError("MerkleTree::make_proof: leaf_index out of range");

    Proof proof;
    std::vector<Hash> level = leaves;
    size_t idx = leaf_index;

    while (level.size() > 1) {
        const bool promoted = (idx + 1 == level.size()) && (level.size() % 2 == 1);
        if (!promoted) {
            if (idx % 2 == 0) {                    // left child, sibling on the right
                proof.path.push_back(level[idx + 1]);
                proof.sibling_is_right.push_back(true);
            } else {                                // right child, sibling on the left
                proof.path.push_back(level[idx - 1]);
                proof.sibling_is_right.push_back(false);
            }
        }
        idx /= 2;
        level = next_level(level);
    }
    return proof;
}

Hash MerkleTree::apply_proof(const Hash& leaf, const Proof& proof) noexcept {
    Hash running = leaf;
    const size_t n = std::min(proof.path.size(), proof.sibling_is_right.size());
    for (size_t i = 0; i < n; ++i) {
        running = proof.sibling_is_right[i]
                    ? node_hash(running, proof.path[i])
                    : node_hash(proof.path[i], running);
    }
    return running;
}

bool MerkleTree::verify(const Hash& leaf, const Proof& proof,
                        const Hash& expected_root) noexcept {
    if (proof.path.size() != proof.sibling_is_right.size())
        return false;
    return apply_proof(leaf, proof) == expected_root;
}

} // namespace blockchain
