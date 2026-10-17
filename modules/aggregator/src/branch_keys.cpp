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

namespace {

std::vector<PublicKey> own_line(const KeyPath& kp) {
    std::vector<PublicKey> line{kp.path.back().working_pubkey};
    for (const Block& r : kp.rotations) {
        PublicKey next{};
        std::copy(r.payload.begin(), r.payload.begin() + 32, next.bytes.begin());
        line.push_back(next);
    }
    return line;
}

} // namespace

std::vector<PublicKey> BranchKeys::keys_for(const Key& k, Timestamp at) const {
    std::vector<PublicKey> keys;
    const auto rev = revocations_.find(k);
    // After the compromise only the line the higher key named signs.
    if (rev != revocations_.end() && at > rev->second.since) return rev->second.line;
    if (const auto it = branches_.find(k); it != branches_.end()) keys = it->second.own;
    if (rev != revocations_.end())
        keys.insert(keys.end(), rev->second.line.begin(), rev->second.line.end());
    return keys;
}

UserId BranchKeys::learn_path(const KeyPath& kp) {
    const UserId chain = KeyPathCert::verify(kp);   // throws on a path that does not check
    const Key    k{chain, kp.path.back().index};
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, fresh] = branches_.try_emplace(k);
    auto& known = it->second;
    if (!fresh) {
        // Another node at the same place is a fork of the tree (blockchain.md
        // §9.6), and a line of key changes that parts from the known one is a
        // fork of the branch — the same rank either way: the first stays, and an
        // ancestor's revocation decides. A path replaces the known one only by
        // carrying it further.
        if (!(kp.path.back().working_pubkey == known.path.path.back().working_pubkey)) return chain;
        if (kp.rotations.size() <= known.path.rotations.size()) return chain;
        for (std::size_t i = 0; i < known.path.rotations.size(); ++i)
            if (Crypto::hash_block(kp.rotations[i]) != Crypto::hash_block(known.path.rotations[i]))
                return chain;
    }
    known.path = kp;
    known.own  = own_line(kp);
    return chain;
}

void BranchKeys::learn_rotation(const Block& b) {
    if (b.type != BlockType::KEY_ROTATION || b.payload.size() < 32) return;
    const Key k{b.address.user_id, b.address.node_index};
    PublicKey next{};
    std::copy(b.payload.begin(), b.payload.begin() + 32, next.bytes.begin());
    std::lock_guard<std::mutex> lock(mu_);
    // After the compromise the replacement's line goes on, not the branch's own.
    if (const auto rev = revocations_.find(k);
        rev != revocations_.end() && b.timestamp_claimed > rev->second.since) {
        auto& line = rev->second.line;
        if (!line.empty() && signed_by(b, line.back())) line.push_back(next);
        return;
    }
    const auto it = branches_.find(k);
    if (it == branches_.end()) return;
    auto& br = it->second;
    if (!br.path.rotations.empty() &&
        b.address.block_index <= br.path.rotations.back().address.block_index)
        return;                                              // known already, or out of turn
    if (!signed_by(b, br.own.back())) return;
    br.path.rotations.push_back(b);
    br.own.push_back(next);
}

void BranchKeys::learn_revocation(const RevocationCertificate& cert) {
    RevocationCert::verify(cert);   // throws on a certificate that does not check
    const auto p = RevocationCert::payload(cert);
    const Key  k{cert.block.address.user_id, p.revoked_node_index};
    Revocation r;
    r.author_depth = node_depth(cert.block.address.node_index);
    r.block        = cert.block.address.block_index;
    r.since        = p.compromised_since;
    r.replacement  = p.replacement_pubkey;
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, fresh] = revocations_.try_emplace(k, r);
    auto& cur = it->second;
    if (!fresh) {
        if (r.author_depth > cur.author_depth) return;      // a lower ancestor: the higher one decided
        if (r.author_depth < cur.author_depth) {
            cur = r;                                         // the higher ancestor decides
        } else {
            cur.since = std::min(cur.since, r.since);        // the same author: the earliest compromise
            if (r.block > cur.block) {                       // and its latest word on the replacement
                cur.block       = r.block;
                cur.replacement = r.replacement;
                cur.line.clear();
            }
        }
    }
    if (cur.line.empty() && cur.replacement) cur.line.push_back(*cur.replacement);
}

BranchKeys::Verdict BranchKeys::check(const Block& b) const {
    std::vector<PublicKey> keys;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const Key k{b.address.user_id, b.address.node_index};
        if (!branches_.count(k) && !revocations_.count(k)) return Verdict::unknown_branch;
        keys = keys_for(k, b.timestamp_claimed);
    }
    for (const auto& key : keys)
        if (signed_by(b, key)) return Verdict::signed_by_branch;
    return Verdict::foreign_signature;
}

bool BranchKeys::vouches(const UserId& chain, NodeIndex node, const Hash& message,
                         const Signature& sig) const {
    std::vector<PublicKey> keys;
    {
        std::lock_guard<std::mutex> lock(mu_);
        const Key k{chain, node};
        if (const auto it = branches_.find(k); it != branches_.end()) keys = it->second.own;
        if (const auto rev = revocations_.find(k); rev != revocations_.end())
            keys.insert(keys.end(), rev->second.line.begin(), rev->second.line.end());
    }
    for (const auto& key : keys)
        if (Crypto::verify(message.bytes.data(), message.bytes.size(), sig, key)) return true;
    return false;
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
