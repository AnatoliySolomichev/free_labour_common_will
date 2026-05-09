#include "blockchain/blockchain.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <algorithm>

namespace blockchain {
namespace {

// Build a signed node. parent_sec signs the canonical CBOR (with parent_sig zeroed).
Node build_signed_node(NodeIndex idx, const KeyPair& kp,
                       const Hash& parent_hash, const SecretKey& parent_sec,
                       Timestamp created_at) {
    Node n{};
    n.index              = idx;
    n.structural_pubkey  = kp.pub;
    n.working_pubkey     = kp.pub; // MVP: structural == working
    n.parent_hash        = parent_hash;
    n.created_at         = created_at;
    n.parent_sig         = Signature::null();
    auto bytes           = Serializer::encode(n);
    n.parent_sig         = Crypto::sign(bytes.data(), bytes.size(), parent_sec);
    return n;
}

// Build a signed block. signature covers the CBOR encoding with signature zeroed.
Block build_signed_block(const BlockAddress& addr, const Hash& prev_hash,
                         Timestamp ts, BlockType type,
                         std::vector<uint8_t> payload,
                         const SecretKey& signing_sec) {
    Block b{};
    b.address           = addr;
    b.prev_hash         = prev_hash;
    b.timestamp_claimed = ts;
    b.type              = type;
    b.payload           = std::move(payload);
    b.signature         = Signature::null();
    auto bytes          = Serializer::encode(b);
    b.signature         = Crypto::sign(bytes.data(), bytes.size(), signing_sec);
    return b;
}

} // anonymous namespace

Blockchain::Blockchain(IStorage& storage, Validator& validator)
    : storage_(storage), validator_(validator) {}

// ── Identity ──────────────────────────────────────────────────────────────────

Node Blockchain::create_identity(const KeyPair& root_keypair) {
    const UserId& uid = root_keypair.pub;
    if (storage_.has_node(uid, 0))
        throw InvalidArgumentError("identity already exists for this key");
    // Root: parent_hash = zero, self-signed (parent_sec = own sec).
    Node n = build_signed_node(0, root_keypair, Hash::zero(), root_keypair.sec, 0);
    storage_.put_node(uid, n);
    return n;
}

// ── Node tree ─────────────────────────────────────────────────────────────────

std::vector<Node> Blockchain::ensure_path(
    const UserId&                     user_id,
    NodeIndex                         leaf_index,
    std::function<KeyPair(NodeIndex)> key_for)
{
    if (!is_leaf_node(leaf_index))
        throw InvalidArgumentError("ensure_path: node is not a leaf (depth != 31)");
    if (!storage_.has_node(user_id, 0))
        throw NodeNotFoundError(0);

    auto indices = path_indices(leaf_index); // root → leaf, inclusive
    std::vector<Node> created;

    for (NodeIndex idx : indices) {
        if (storage_.has_node(user_id, idx)) continue;

        KeyPair kp         = key_for(idx);
        NodeIndex par_idx  = (idx - 1) / 2;
        Node parent        = storage_.get_node(user_id, par_idx);
        Hash ph            = Crypto::hash_node(parent);
        KeyPair parent_kp  = key_for(par_idx);

        Node n = build_signed_node(idx, kp, ph, parent_kp.sec, 0);
        storage_.put_node(user_id, n);
        created.push_back(n);
    }
    return created;
}

Node Blockchain::get_node(const UserId& user_id, NodeIndex index) const {
    return storage_.get_node(user_id, index);
}

std::vector<Node> Blockchain::get_path(const UserId& user_id,
                                        NodeIndex leaf_index) const {
    auto indices = path_indices(leaf_index);
    std::vector<Node> path;
    path.reserve(indices.size());
    for (NodeIndex idx : indices)
        path.push_back(storage_.get_node(user_id, idx));
    return path;
}

// ── Branch helpers ────────────────────────────────────────────────────────────

Hash Blockchain::branch_tip_hash(const UserId& user_id,
                                  NodeIndex leaf_index) const {
    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value()) {
        Node leaf = storage_.get_node(user_id, leaf_index);
        return Crypto::hash_node(leaf);
    }
    Block b = storage_.get_block({user_id, leaf_index, *tip_opt});
    return Crypto::hash_block(b);
}

// ── Block operations ──────────────────────────────────────────────────────────

Block Blockchain::append_data_block(
    const UserId&        user_id,
    NodeIndex            leaf_index,
    std::vector<uint8_t> payload,
    const KeyPair&       working_keypair,
    Timestamp            timestamp)
{
    if (!is_leaf_node(leaf_index))
        throw InvalidArgumentError("append_data_block: node is not a leaf (depth != 31)");

    Hash prev_hash = branch_tip_hash(user_id, leaf_index);
    auto tip_opt   = storage_.branch_tip_index(user_id, leaf_index);
    BlockIndex idx = tip_opt.has_value() ? (*tip_opt + 1) : 0;

    Block b = build_signed_block(
        {user_id, leaf_index, idx}, prev_hash, timestamp,
        BlockType::DATA, std::move(payload), working_keypair.sec);
    storage_.put_block(b);
    return b;
}

Block Blockchain::rotate_key(
    const UserId&  user_id,
    NodeIndex      leaf_index,
    const KeyPair& old_working_keypair,
    const KeyPair& new_keypair,
    Timestamp      timestamp)
{
    Hash prev_hash = branch_tip_hash(user_id, leaf_index);
    auto tip_opt   = storage_.branch_tip_index(user_id, leaf_index);
    BlockIndex idx = tip_opt.has_value() ? (*tip_opt + 1) : 0;

    // KEY_ROTATION payload: raw 32-byte new working pubkey.
    std::vector<uint8_t> payload(new_keypair.pub.bytes.begin(),
                                  new_keypair.pub.bytes.end());
    Block b = build_signed_block(
        {user_id, leaf_index, idx}, prev_hash, timestamp,
        BlockType::KEY_ROTATION, std::move(payload), old_working_keypair.sec);
    storage_.put_block(b);
    return b;
}

Block Blockchain::revoke_node(
    const UserId&    user_id,
    NodeIndex        revoked_node_index,
    Timestamp        compromised_since,
    const PublicKey& replacement_pubkey,
    const KeyPair&   parent_keypair,
    Timestamp        timestamp)
{
    // [OPEN §11.1] Placed in the revoking parent node's service branch.
    NodeIndex par_node_idx = (revoked_node_index - 1) / 2;

    Hash prev_hash;
    auto tip_opt = storage_.branch_tip_index(user_id, par_node_idx);
    if (!tip_opt.has_value()) {
        Node par = storage_.get_node(user_id, par_node_idx);
        prev_hash = Crypto::hash_node(par);
    } else {
        prev_hash = Crypto::hash_block(
            storage_.get_block({user_id, par_node_idx, *tip_opt}));
    }
    BlockIndex idx = tip_opt.has_value() ? (*tip_opt + 1) : 0;

    // RevocationPayload (raw): 4B revoked_node_index BE + 8B compromised_since BE
    //                          + 32B replacement_pubkey = 44 bytes.
    std::vector<uint8_t> payload(44);
    uint32_t ri = revoked_node_index;
    payload[0] = (ri >> 24) & 0xFF; payload[1] = (ri >> 16) & 0xFF;
    payload[2] = (ri >>  8) & 0xFF; payload[3] =  ri        & 0xFF;
    uint64_t cs = static_cast<uint64_t>(compromised_since);
    for (int i = 0; i < 8; ++i) payload[4 + i] = (cs >> (56 - 8 * i)) & 0xFF;
    std::copy(replacement_pubkey.bytes.begin(), replacement_pubkey.bytes.end(),
              payload.begin() + 12);

    Block b = build_signed_block(
        {user_id, par_node_idx, idx}, prev_hash, timestamp,
        BlockType::REVOCATION, std::move(payload), parent_keypair.sec);
    storage_.put_block(b);
    return b;
}

Block Blockchain::get_block(const BlockAddress& address) const {
    return storage_.get_block(address);
}

std::vector<Block> Blockchain::get_branch(const UserId& user_id,
                                           NodeIndex leaf_index) const {
    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value()) return {};

    std::vector<Block> branch;
    branch.reserve(*tip_opt + 1);
    for (BlockIndex i = 0; i <= *tip_opt; ++i)
        branch.push_back(storage_.get_block({user_id, leaf_index, i}));
    return branch;
}

// ── Validation (delegates to Validator) ───────────────────────────────────────

void Blockchain::validate_path(const UserId& user_id, NodeIndex leaf_index) const {
    validator_.validate_path(user_id, leaf_index);
}

void Blockchain::validate_branch(const UserId& user_id, NodeIndex leaf_index) const {
    validator_.validate_branch(user_id, leaf_index);
}

} // namespace blockchain
