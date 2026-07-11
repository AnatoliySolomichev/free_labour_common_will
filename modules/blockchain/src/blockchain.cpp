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
    // Branches may grow from any node of the tree (blockchain.md §3.2 v0.7);
    // only the heap index itself must be valid (depth ≤ 31).
    if (!is_valid_node(leaf_index))
        throw InvalidArgumentError("ensure_path: invalid node index (depth > 31)");
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

void Blockchain::ensure_branch_writable(const UserId& user_id, NodeIndex node_index,
                                        const PublicKey& signing_pubkey) const {
    // Fast path: no revocation — nothing to enforce (the common case).
    if (!validator_.effective_revocation(user_id, node_index).has_value()) return;

    auto st = validator_.branch_revocation_status(user_id, node_index);
    if (st.state == BranchRevocationState::FROZEN)
        throw RevocationError(
            "branch is frozen by revocation — no replacement assigned (§6.7)");
    if (st.state == BranchRevocationState::REPLACED
        && st.next_key.has_value() && signing_pubkey != *st.next_key)
        throw RevocationError(
            "branch key was replaced by revocation; signing key is not the authorized one");
}

Block Blockchain::append_data_block(
    const UserId&        user_id,
    NodeIndex            leaf_index,
    std::vector<uint8_t> payload,
    const KeyPair&       working_keypair,
    Timestamp            timestamp)
{
    if (!is_valid_node(leaf_index))
        throw InvalidArgumentError("append_data_block: invalid node index (depth > 31)");
    ensure_branch_writable(user_id, leaf_index, working_keypair.pub);

    Hash prev_hash = branch_tip_hash(user_id, leaf_index);
    auto tip_opt   = storage_.branch_tip_index(user_id, leaf_index);
    BlockIndex idx = tip_opt.has_value() ? (*tip_opt + 1) : 0;

    Block b = build_signed_block(
        {user_id, leaf_index, idx}, prev_hash, timestamp,
        BlockType::DATA, std::move(payload), working_keypair.sec);
    storage_.put_block(b);
    return b;
}

Block Blockchain::append_stub_block(
    const UserId&  user_id,
    NodeIndex      leaf_index,
    const KeyPair& working_keypair,
    Timestamp      timestamp)
{
    // Empty CBOR array (0x80) = "zero records" (records.md §2): valid CBOR that
    // carries no user data. The block exists only for its structural / timing role.
    return append_data_block(user_id, leaf_index, {0x80}, working_keypair, timestamp);
}

Block Blockchain::rotate_key(
    const UserId&  user_id,
    NodeIndex      leaf_index,
    const KeyPair& old_working_keypair,
    const KeyPair& new_keypair,
    Timestamp      timestamp)
{
    ensure_branch_writable(user_id, leaf_index, old_working_keypair.pub);

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

PublicKey Blockchain::effective_working_pubkey(const UserId& user_id,
                                               NodeIndex node_index) const {
    Node node = storage_.get_node(user_id, node_index);
    PublicKey key = node.working_pubkey;

    auto tip_opt = storage_.branch_tip_index(user_id, node_index);
    if (!tip_opt.has_value()) return key;

    for (BlockIndex i = 0; i <= *tip_opt; ++i) {
        Block b = storage_.get_block({user_id, node_index, i});
        if (b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32)
            std::copy(b.payload.begin(), b.payload.begin() + 32, key.bytes.begin());
    }
    return key;
}

Block Blockchain::revoke_node(
    const UserId&                   user_id,
    NodeIndex                       revoked_node_index,
    Timestamp                       compromised_since,
    const std::optional<PublicKey>& replacement_pubkey,
    NodeIndex                       ancestor_index,
    const KeyPair&                  ancestor_keypair,
    Timestamp                       timestamp)
{
    if (revoked_node_index == 0)
        throw RevocationError("root cannot be revoked — it has no ancestor (§11.5)");
    if (!is_valid_node(revoked_node_index))
        throw InvalidArgumentError("revoke_node: invalid revoked node index");
    // Covers self-revocation: a key cannot revoke itself (§6.7 rule 2).
    if (!is_ancestor(ancestor_index, revoked_node_index))
        throw RevocationError("revocation author is not a strict ancestor of the revoked node");

    // The exact key being revoked: the currently authorized key of the revoked
    // branch (rotation lineage or an earlier replacement); for a frozen branch —
    // the last key of its lineage.
    auto target_st = validator_.branch_revocation_status(user_id, revoked_node_index);
    PublicKey revoked_key = target_st.next_key.has_value()
        ? *target_st.next_key
        : effective_working_pubkey(user_id, revoked_node_index);

    // The author branch itself must be writable with this keypair (it may have
    // been replaced by its own revocation — then only the replacement signs).
    auto author_st = validator_.branch_revocation_status(user_id, ancestor_index);
    if (author_st.state == BranchRevocationState::FROZEN)
        throw RevocationError("revoke_node: author branch is frozen by revocation");
    if (!author_st.next_key.has_value() || ancestor_keypair.pub != *author_st.next_key)
        throw InvalidArgumentError(
            "revoke_node: keypair does not match the ancestor branch working key");

    Hash prev_hash = branch_tip_hash(user_id, ancestor_index);
    auto tip_opt   = storage_.branch_tip_index(user_id, ancestor_index);
    BlockIndex idx = tip_opt.has_value() ? (*tip_opt + 1) : 0;

    RevocationPayload rp{};
    rp.revoked_node_index = revoked_node_index;
    rp.revoked_pubkey     = revoked_key;
    rp.compromised_since  = compromised_since;
    rp.replacement_pubkey = replacement_pubkey;

    Block b = build_signed_block(
        {user_id, ancestor_index, idx}, prev_hash, timestamp,
        BlockType::REVOCATION, Serializer::encode(rp), ancestor_keypair.sec);
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
