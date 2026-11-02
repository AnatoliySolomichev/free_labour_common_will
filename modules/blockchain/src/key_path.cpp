#include "blockchain/key_path.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"

#include <algorithm>
#include <optional>

namespace blockchain {

namespace {

bool signed_by(const Block& block, const PublicKey& key) {
    Block to_verify = block;
    to_verify.signature = Signature::null();
    const auto bytes = Serializer::encode(to_verify);
    return Crypto::verify(bytes.data(), bytes.size(), block.signature, key);
}

// Новый рабочий ключ из payload блока смены (blockchain.md §6.6): 32 сырых байта.
std::optional<PublicKey> rotated_key(const Block& b) {
    if (b.type != BlockType::KEY_ROTATION || b.payload.size() < 32)
        return std::nullopt;
    PublicKey k{};
    std::copy(b.payload.begin(), b.payload.begin() + 32, k.bytes.begin());
    return k;
}

// One path, walked: the nodes its births make and the keys its changes carry.
class Walk {
public:
    Walk(const KeyPath& kp, std::vector<Node> nodes)
        : kp_(kp), chain_(kp.root.structural_pubkey), nodes_(std::move(nodes)) {}

    const Node* find(NodeIndex idx) const {
        for (const Node& n : nodes_) if (n.index == idx) return &n;
        return nullptr;
    }

    // The key `idx` signs with at `place` of its branch, every change before
    // that place checked (blockchain.md §6.6, blockchain.md §6.7 rule 9).
    PublicKey key_at(NodeIndex idx, BlockIndex place, int depth) const {
        if (depth > 64) throw ChainIntegrityError("key path: records refer in a loop");
        const Node* n = find(idx);
        if (!n) throw ChainIntegrityError("key path: node " + std::to_string(idx) +
                                          " is not on the path");
        PublicKey              key = n->working_pubkey;
        std::vector<PublicKey> epoch{key};   // keys of the epoch in force
        std::optional<BlockIndex> prev;
        for (const Block& c : kp_.changes) {
            if (c.address.node_index != idx) continue;
            if (!(c.address.user_id == chain_))
                throw ChainIntegrityError("key path: a key change of another chain");
            if (prev && c.address.block_index <= *prev)
                throw ChainIntegrityError("key path: key changes out of branch order");
            prev = c.address.block_index;
            if (c.address.block_index >= place) break;

            if (c.type == BlockType::KEY_ROTATION) {
                if (!signed_by(c, key))
                    throw SignatureError("key path: a key rotation not signed by the key before it");
                const auto next = rotated_key(c);
                if (!next) throw ChainIntegrityError("key path: a key rotation names no key");
                key = *next;
                epoch.push_back(key);
            } else if (c.type == BlockType::KEY_REPLACEMENT) {
                key = replacement(c, idx, epoch, depth);
                epoch = {key};
            } else {
                throw ChainIntegrityError("key path: a change that is not a key change");
            }
        }
        return key;
    }

private:
    // A replacement block checks against the record it names; returns the key it brings.
    PublicKey replacement(const Block& c, NodeIndex idx, const std::vector<PublicKey>& epoch,
                          int depth) const {
        const ReplacementPayload p =
            Serializer::decode_replacement_payload(c.payload.data(), c.payload.size());
        const auto rec = std::find_if(kp_.records.begin(), kp_.records.end(), [&](const Block& r) {
            return r.address == p.record_address && Crypto::hash_block(r) == p.record_hash;
        });
        if (rec == kp_.records.end())
            throw ChainIntegrityError("key path: the record a replacement names is not carried");
        if (rec->type != BlockType::REVOCATION || !(rec->address.user_id == chain_))
            throw ChainIntegrityError("key path: a replacement names no revocation of this chain");
        const NodeIndex author = rec->address.node_index;
        if (!is_ancestor(author, idx))
            throw ChainIntegrityError("key path: a record's author is not an ancestor of the node");
        const RevocationPayload rp =
            Serializer::decode_revocation_payload(rec->payload.data(), rec->payload.size());
        if (rp.revoked_node_index != idx || !rp.replacement_pubkey.has_value())
            throw ChainIntegrityError("key path: the record assigns no replacement to this node");
        if (std::find(epoch.begin(), epoch.end(), rp.revoked_pubkey) == epoch.end())
            throw ChainIntegrityError("key path: the record revokes a key the node did not have");
        if (!signed_by(c, *rp.replacement_pubkey))
            throw SignatureError("key path: a replacement block not signed by the replacement");
        if (rp.last_good.has_value()) {
            const LastGood&  line = *rp.last_good;
            const BlockIndex at   = line.index == EMPTY_BRANCH_INDEX ? 0 : line.index + 1;
            if (c.address.block_index != at || c.prev_hash != line.hash)
                throw ChainIntegrityError(
                    "key path: a replacement block not right after the line its record names");
        }
        if (!signed_by(*rec, key_at(author, rec->address.block_index, depth + 1)))
            throw SignatureError("key path: a record not signed by its author's key at its place");
        return *rp.replacement_pubkey;
    }

    const KeyPath&    kp_;
    UserId            chain_;
    std::vector<Node> nodes_;
};

} // namespace

Hash branch_origin(const Node& node) {
    return node.index == 0 ? Crypto::hash_node(node) : node.birth.hash;
}

Node node_born(const Block& birth) {
    if (birth.type != BlockType::BIRTH)
        throw InvalidArgumentError("node_born: not a BIRTH block");
    const BirthPayload p =
        Serializer::decode_birth_payload(birth.payload.data(), birth.payload.size());
    Node n{};
    n.index             = p.child_index;
    n.structural_pubkey = p.structural_pubkey;
    n.working_pubkey    = p.working_pubkey;
    n.birth             = BirthRef{birth.address.block_index, Crypto::hash_block(birth)};
    n.root_sig          = Signature::null();
    return n;
}

std::vector<Block> births_of(const IStorage& storage, const UserId& chain, NodeIndex node) {
    std::vector<Block> out;
    for (NodeIndex idx : path_indices(node)) {
        if (idx == 0) continue;
        const Node         n = storage.get_node(chain, idx);
        const BlockAddress at{chain, (idx - 1) / 2, n.birth.index};
        out.push_back(storage.has_block(at) ? storage.get_block(at)
                                            : storage.get_external_block(at));
    }
    return out;
}

std::vector<Block> key_changes_of(const IStorage& storage, const UserId& chain, NodeIndex node) {
    std::vector<Block> out;
    const auto tip = storage.branch_tip_index(chain, node);
    if (!tip.has_value()) return out;
    for (BlockIndex i = 0; i <= *tip; ++i) {
        const BlockAddress at{chain, node, i};
        if (!storage.has_block(at)) continue;                 // a gap is no trouble
        Block b = storage.get_block(at);
        if (b.type == BlockType::KEY_ROTATION || b.type == BlockType::KEY_REPLACEMENT)
            out.push_back(std::move(b));
    }
    return out;
}

KeyPath KeyPathCert::build(const IStorage& storage, const UserId& chain, NodeIndex node) {
    KeyPath kp{};
    kp.root   = storage.get_node(chain, 0);                   // NodeNotFoundError
    kp.births = births_of(storage, chain, node);
    for (NodeIndex idx : path_indices(node))
        for (Block& c : key_changes_of(storage, chain, idx)) {
            if (c.type == BlockType::KEY_REPLACEMENT) {
                // The record that assigned the key goes along (blockchain.md §6.7 rule 9).
                try {
                    const ReplacementPayload p =
                        Serializer::decode_replacement_payload(c.payload.data(), c.payload.size());
                    const BlockAddress& at = p.record_address;
                    if (storage.has_block(at) || storage.has_external_block(at)) {
                        Block rec = storage.has_block(at) ? storage.get_block(at)
                                                          : storage.get_external_block(at);
                        const bool known = std::any_of(kp.records.begin(), kp.records.end(),
                            [&](const Block& r) { return r.address == rec.address; });
                        if (!known && Crypto::hash_block(rec) == p.record_hash)
                            kp.records.push_back(std::move(rec));
                    }
                } catch (const SerializationError&) {}         // the reader refuses it anyway
            }
            kp.changes.push_back(std::move(c));
        }
    return kp;
}

std::vector<Node> KeyPathCert::nodes(const KeyPath& kp) {
    std::vector<Node> out{kp.root};
    for (const Block& b : kp.births) out.push_back(node_born(b));
    return out;
}

NodeIndex KeyPathCert::node(const KeyPath& kp) {
    return kp.births.empty() ? kp.root.index : node_born(kp.births.back()).index;
}

UserId KeyPathCert::verify(const KeyPath& kp) {
    const Node& root = kp.root;
    if (root.structural_pubkey == PublicKey{})
        throw ChainIntegrityError("key path: empty — no root's record");
    if (root.index != 0 || !(root.birth == BirthRef{}))
        throw ChainIntegrityError("key path: does not start at the root's record");
    Node to_sign = root;
    to_sign.root_sig = Signature::null();
    const auto bytes = Serializer::encode(to_sign);
    if (!Crypto::verify(bytes.data(), bytes.size(), root.root_sig, root.structural_pubkey))
        throw SignatureError("key path: root self-signature invalid");
    const UserId chain = root.structural_pubkey;

    // Births, top down: each one signed by its parent's key at its place.
    std::vector<Node> path{root};
    for (const Block& birth : kp.births) {
        const Node& parent = path.back();
        if (birth.type != BlockType::BIRTH)
            throw ChainIntegrityError("key path: a birth that is not a BIRTH block");
        if (!(birth.address.user_id == chain) || birth.address.node_index != parent.index)
            throw ChainIntegrityError("key path: a birth outside the parent's branch");
        const Node child = node_born(birth);                 // SerializationError
        if (child.index == 0 || child.parent_index() != parent.index)
            throw ChainIntegrityError("key path: indices are not parent→child");
        const Walk w(kp, path);
        if (!signed_by(birth, w.key_at(parent.index, birth.address.block_index, 0)))
            throw SignatureError("key path: a birth not signed by the parent's key at its place");
        path.push_back(child);
    }

    // Every key change belongs to a node of the path and checks through.
    const Walk w(kp, path);
    for (const Block& c : kp.changes)
        if (!w.find(c.address.node_index))
            throw ChainIntegrityError("key path: a key change of a node off the path");
    for (const Node& n : path) w.key_at(n.index, kNow, 0);
    return chain;
}

PublicKey KeyPathCert::key_at(const KeyPath& kp, NodeIndex node, BlockIndex place) {
    return Walk(kp, nodes(kp)).key_at(node, place, 0);
}

PublicKey KeyPathCert::signer_key(const KeyPath& kp) {
    return key_at(kp, node(kp), kNow);
}

} // namespace blockchain
