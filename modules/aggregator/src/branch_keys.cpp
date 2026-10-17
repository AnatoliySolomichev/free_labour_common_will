#include "aggregator/branch_keys.h"

#include <blockchain/crypto.h>
#include <blockchain/errors.h>
#include <blockchain/serializer.h>

#include <algorithm>

namespace aggregator {

namespace {

bool signed_by(const Block& block, const PublicKey& key) {
    Block to_verify     = block;
    to_verify.signature = Signature::null();
    const auto bytes    = Serializer::encode(to_verify);
    return Crypto::verify(bytes.data(), bytes.size(), block.signature, key);
}

} // namespace

void BranchKeys::rekey(const Key& k, Branch& b) {
    b.keys.clear();
    b.keys.push_back(b.path.path.back().working_pubkey);
    for (const Block& r : b.path.rotations) {
        PublicKey next{};
        std::copy(r.payload.begin(), r.payload.begin() + 32, next.bytes.begin());
        b.keys.push_back(next);
    }
    if (const auto it = replacements_.find(k); it != replacements_.end())
        b.keys.insert(b.keys.end(), it->second.begin(), it->second.end());
}

UserId BranchKeys::learn_path(const KeyPath& kp) {
    const UserId chain = KeyPathCert::verify(kp);   // throws on a path that does not check
    const Key    k{chain, kp.path.back().index};
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, fresh] = branches_.try_emplace(k);
    // Of two checked paths of one branch the one that knows more key changes wins:
    // both start at the same node key, and each change is signed by the one before.
    // A path to another node at the same place is a fork of the tree
    // (blockchain.md §9.6): the first one stays.
    if (!fresh && !(kp.path.back().working_pubkey == it->second.path.path.back().working_pubkey))
        return chain;
    if (fresh || kp.rotations.size() > it->second.path.rotations.size()) it->second.path = kp;
    rekey(k, it->second);
    return chain;
}

void BranchKeys::learn_rotation(const Block& b) {
    if (b.type != BlockType::KEY_ROTATION || b.payload.size() < 32) return;
    const Key k{b.address.user_id, b.address.node_index};
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = branches_.find(k);
    if (it == branches_.end()) return;
    auto& br = it->second;
    if (!br.path.rotations.empty() &&
        b.address.block_index <= br.path.rotations.back().address.block_index)
        return;                                              // known already, or out of turn
    if (!signed_by(b, KeyPathCert::signer_key(br.path))) return;
    br.path.rotations.push_back(b);
    rekey(k, br);
}

void BranchKeys::learn_revocation(const RevocationCertificate& cert) {
    RevocationCert::verify(cert);   // throws on a certificate that does not check
    const auto p = RevocationCert::payload(cert);
    if (!p.replacement_pubkey) return;                       // a stop: nothing new signs
    const Key k{cert.block.address.user_id, p.revoked_node_index};
    std::lock_guard<std::mutex> lock(mu_);
    auto& named = replacements_[k];
    if (std::find(named.begin(), named.end(), *p.replacement_pubkey) != named.end()) return;
    named.push_back(*p.replacement_pubkey);
    if (const auto it = branches_.find(k); it != branches_.end()) rekey(k, it->second);
}

BranchKeys::Verdict BranchKeys::check(const Block& b) const {
    std::vector<PublicKey> keys;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const auto it = branches_.find({b.address.user_id, b.address.node_index});
        if (it == branches_.end()) return Verdict::unknown_branch;
        keys = it->second.keys;
    }
    for (const auto& key : keys)
        if (signed_by(b, key)) return Verdict::signed_by_branch;
    return Verdict::foreign_signature;
}

std::optional<KeyPath> BranchKeys::path(const UserId& chain, NodeIndex node) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = branches_.find({chain, node});
    if (it == branches_.end()) return std::nullopt;
    return it->second.path;
}

void BranchKeys::load(const AggregatorStorage& storage) {
    for (const auto& key : storage.all_keypath_keys())
        for (const auto& bytes : storage.get_keypath_bytes(key)) {
            try {
                learn_path(Serializer::decode_key_path(bytes.data(), bytes.size()));
            } catch (const std::exception&) {}               // stored before the filter: skip
        }
    for (const auto& chain : storage.all_revoked_chains())
        for (const auto& bytes : storage.get_revocation_bytes(chain)) {
            try {
                learn_revocation(Serializer::decode_revocation_cert(bytes.data(), bytes.size()));
            } catch (const std::exception&) {}
        }
    // Key changes the paths did not carry, branch by branch, in order.
    std::map<Key, std::map<BlockIndex, Block>> rotations;
    for (const Hash& h : storage.all_block_hashes())
        if (const auto b = storage.get_block_by_hash(h); b && b->type == BlockType::KEY_ROTATION)
            rotations[{b->address.user_id, b->address.node_index}][b->address.block_index] = *b;
    for (const auto& [branch, blocks] : rotations) {
        (void)branch;
        for (const auto& [index, b] : blocks) {
            (void)index;
            learn_rotation(b);
        }
    }
}

} // namespace aggregator
