#include "aggregator/branch_keys.h"

#include <blockchain/crypto.h>
#include <blockchain/errors.h>
#include <blockchain/serializer.h>

#include <algorithm>
#include <limits>

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

namespace {

// Whether `b` lies on the owner's line ending at `line`: prev_hash back through
// the blocks kept here. Where they are not here, the number decides — up to
// the line's end the block is taken for the owner's (a filter, not a proof;
// readers walk the line themselves).
bool on_line(const AggregatorStorage* storage, const Block& b, const LastGood& line) {
    if (line.index == EMPTY_BRANCH_INDEX || b.address.block_index > line.index) return false;
    const Hash mine = Crypto::hash_block(b);
    if (b.address.block_index == line.index) return mine == line.hash;
    if (!storage) return true;                               // nothing to walk: by the number
    Hash at = line.hash;
    for (BlockIndex i = line.index; i > b.address.block_index; --i) {
        const auto step = storage->get_block_by_hash(at);
        if (!step) return true;                              // not here: by the number
        if (step->address.node_index != b.address.node_index ||
            step->address.block_index != i)
            return false;
        at = step->prev_hash;
    }
    return at == mine;
}

} // namespace

bool BranchKeys::admits(const std::vector<Record>& records, const std::vector<PublicKey>& epoch,
                        const Block& b, const AggregatorStorage* storage) {
    bool                    revoked = false;
    std::optional<LastGood> line;
    for (const Record& r : records) {
        if (std::find(epoch.begin(), epoch.end(), r.revoked) == epoch.end()) continue;
        revoked = true;
        if (r.last_good.has_value()) line = r.last_good;   // the latest word on the line
    }
    if (!revoked) return true;
    // Only the block's place decides, never the date its author wrote; a
    // revocation with no line named yet is a full stop.
    return line.has_value() && on_line(storage, b, *line);
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
    // A replacement's own key change carries that replacement's line on.
    if (const auto rev = revocations_.find(k); rev != revocations_.end())
        for (auto& line : rev->second.lines)
            if (signed_by(b, line.back())) {
                if (std::find(line.begin(), line.end(), next) == line.end()) line.push_back(next);
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
    const auto     p     = RevocationCert::payload(cert);
    const Key      k{cert.block.address.user_id, p.revoked_node_index};
    const uint32_t depth = node_depth(cert.block.address.node_index);
    const Record   rec{cert.block.address.block_index, p.revoked_pubkey,
                       p.replacement_pubkey, p.last_good, cert.block,
                       cert.path.back().working_pubkey};
    std::lock_guard<std::mutex> lock(mu_);
    auto [it, fresh] = revocations_.try_emplace(k);
    auto& cur = it->second;
    if (!fresh && depth > cur.author_depth) return;          // a lower ancestor: the higher one decided
    if (fresh || depth < cur.author_depth) {                 // the higher ancestor decides anew
        cur              = Revocation{};
        cur.author_depth = depth;
    }
    // The same author: every record, in its block order — the earliest
    // compromise, the latest word on the replacement and on the line.
    const auto pos = std::find_if(cur.records.begin(), cur.records.end(),
                                  [&](const Record& r) { return r.block >= rec.block; });
    if (pos != cur.records.end() && pos->block == rec.block) return;   // known already
    cur.records.insert(pos, rec);
    if (rec.replacement.has_value() &&
        std::none_of(cur.lines.begin(), cur.lines.end(),
                     [&](const auto& l) { return l.front() == *rec.replacement; }))
        cur.lines.push_back({*rec.replacement});
}

BranchKeys::Verdict BranchKeys::check(const Block& b, const AggregatorStorage* storage) const {
    std::vector<std::vector<PublicKey>> epochs;   // the own line, then each replacement line
    std::vector<Record>                 records;  // only those that count (rule 10)
    {
        std::lock_guard<std::mutex> lock(mu_);
        const Key  k{b.address.user_id, b.address.node_index};
        const auto br  = branches_.find(k);
        const auto rev = revocations_.find(k);
        if (br == branches_.end() && rev == revocations_.end()) return Verdict::unknown_branch;
        if (br != branches_.end()) {
            const auto& births = br->second.path.births;
            if (b.address.block_index == 0 && !births.empty() &&
                b.timestamp_claimed < births.back().timestamp_claimed)
                return Verdict::before_birth;
            if (!born(br->second, storage, 0)) return Verdict::revoked_key;
            epochs.push_back(br->second.own);
        }
        if (rev != revocations_.end()) {
            records = counting(k.first, rev->second, storage, 0);
            for (const auto& line : rev->second.lines)
                if (std::any_of(records.begin(), records.end(), [&](const Record& r) {
                        return r.replacement && *r.replacement == line.front();
                    }))
                    epochs.push_back(line);
        }
    }
    bool revoked = false;
    for (const auto& epoch : epochs)
        for (const auto& key : epoch)
            if (signed_by(b, key)) {
                if (admits(records, epoch, b, storage)) return Verdict::signed_by_branch;
                revoked = true;
            }
    return revoked ? Verdict::revoked_key : Verdict::foreign_signature;
}

std::vector<BranchKeys::Record> BranchKeys::counting(const UserId& chain, const Revocation& rev,
                                                     const AggregatorStorage* storage,
                                                     int depth) const {
    std::vector<Record> out;
    for (const Record& r : rev.records) {
        const Key author{chain, r.written.address.node_index};
        // The author is not under revocation itself — the record counts as is.
        if (!revocations_.count(author)) { out.push_back(r); continue; }
        const std::vector<PublicKey> own{r.author_key};
        const auto br = branches_.find(author);
        if (whose_locked(r.written, br != branches_.end() ? nullptr : &own, storage, depth + 1)
            == Whose::owner)
            out.push_back(r);
    }
    return out;
}

bool BranchKeys::born(const Branch& br, const AggregatorStorage* storage, int depth) const {
    const auto& path = br.path.path;
    for (std::size_t k = 0; k < br.path.births.size() && k + 1 < path.size(); ++k) {
        const Block& birth = br.path.births[k];
        if (!revocations_.count({birth.address.user_id, path[k].index})) continue;
        const std::vector<PublicKey> own{path[k].working_pubkey};
        if (whose_locked(birth, &own, storage, depth + 1) == Whose::not_owner) return false;
    }
    return true;
}

BranchKeys::Whose BranchKeys::whose_locked(const Block& b, const std::vector<PublicKey>* own,
                                           const AggregatorStorage* storage, int depth) const {
    if (depth > 64) return Whose::unknown;                   // deeper than any tree
    const Key  k{b.address.user_id, b.address.node_index};
    const auto br  = branches_.find(k);
    const auto rev = revocations_.find(k);
    if (br == branches_.end() && rev == revocations_.end() && !own) return Whose::unknown;
    // `own` marks a check of a birth or a record: the path is being walked already.
    if (!own && br != branches_.end() && !born(br->second, storage, depth)) return Whose::not_owner;
    std::vector<Record> records;
    if (rev != revocations_.end()) records = counting(k.first, rev->second, storage, depth);
    // The epochs: the own line, then each replacement a counting record named.
    std::vector<const std::vector<PublicKey>*> epochs;
    if (br != branches_.end())  epochs.push_back(&br->second.own);
    else if (own)               epochs.push_back(own);
    if (rev != revocations_.end())
        for (const auto& line : rev->second.lines)
            if (std::any_of(records.begin(), records.end(), [&](const Record& r) {
                    return r.replacement && *r.replacement == line.front();
                }))
                epochs.push_back(&line);
    for (const auto* epoch : epochs)
        for (const auto& key : *epoch) {
            if (!signed_by(b, key)) continue;
            return admits(records, *epoch, b, storage) ? Whose::owner : Whose::not_owner;
        }
    return Whose::not_owner;   // a key no counting record gave this branch
}

BranchKeys::Whose BranchKeys::whose(const Block& b, const AggregatorStorage& storage) const {
    std::lock_guard<std::mutex> lock(mu_);
    return whose_locked(b, nullptr, &storage, 0);
}

std::optional<KeyPath> BranchKeys::path(const UserId& chain, NodeIndex node) const {
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = branches_.find({chain, node});
    if (it == branches_.end()) return std::nullopt;
    return it->second.path;
}

void BranchKeys::load(const AggregatorStorage& storage, const std::set<UserId>* only) {
    const auto wanted = [&](const UserId& chain) { return !only || only->count(chain) > 0; };
    for (const auto& key : storage.all_keypath_keys())
        for (const auto& bytes : storage.get_keypath_bytes(key)) {
            try {
                const KeyPath kp = Serializer::decode_key_path(bytes.data(), bytes.size());
                if (!kp.path.empty() && wanted(kp.path.front().structural_pubkey)) learn_path(kp);
            } catch (const std::exception&) {}               // stored before the filter: skip
        }
    for (const auto& chain : storage.all_revoked_chains()) {
        if (!wanted(chain)) continue;
        for (const auto& bytes : storage.get_revocation_bytes(chain)) {
            try {
                learn_revocation(Serializer::decode_revocation_cert(bytes.data(), bytes.size()));
            } catch (const std::exception&) {}
        }
    }
    // Key changes the paths did not carry, branch by branch, in order.
    std::map<Key, std::map<BlockIndex, Block>> rotations;
    for (const Hash& h : storage.all_block_hashes())
        if (const auto b = storage.get_block_by_hash(h);
            b && b->type == BlockType::KEY_ROTATION && wanted(b->address.user_id))
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
