#include "blockchain/blockchain.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include "blockchain/key_path.h"
#include <algorithm>

namespace blockchain {
namespace {


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
    // The root has no parent: its record is signed by itself (blockchain.md §6.1).
    Node n{};
    n.index             = 0;
    n.structural_pubkey = root_keypair.pub;
    n.working_pubkey    = root_keypair.pub;   // MVP: structural == working
    n.birth             = BirthRef{};
    n.root_sig          = Signature::null();
    const auto bytes    = Serializer::encode(n);
    n.root_sig          = Crypto::sign(bytes.data(), bytes.size(), root_keypair.sec);
    storage_.put_node(uid, n);
    return n;
}

// ── Node tree ─────────────────────────────────────────────────────────────────

std::vector<Node> Blockchain::ensure_path(
    const UserId&                     user_id,
    NodeIndex                         leaf_index,
    std::function<KeyPair(NodeIndex)> key_for,
    Timestamp                         timestamp)
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
        KeyPair parent_kp  = key_for(par_idx);

        // Born by a block in the parent's branch (blockchain.md §11.2), wherever
        // that branch stands now; time there only goes forward (blockchain.md §9.4).
        ensure_branch_writable(user_id, par_idx, parent_kp.pub);
        const auto tip_opt = storage_.branch_tip_index(user_id, par_idx);
        const BlockIndex at = tip_opt.has_value() ? (*tip_opt + 1) : 0;
        Timestamp born = timestamp;
        if (tip_opt.has_value())
            born = std::max(born, storage_.get_block({user_id, par_idx, *tip_opt}).timestamp_claimed);
        const Block birth = build_signed_block(
            {user_id, par_idx, at}, branch_tip_hash(user_id, par_idx), born, BlockType::BIRTH,
            Serializer::encode(BirthPayload{idx, kp.pub, kp.pub}), parent_kp.sec);
        storage_.put_block(birth);

        // The node is what its birth says, signed once — by the parent, in the
        // birth block (решение участника 2026-11-02).
        Node n = node_born(birth);
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
    if (!tip_opt.has_value())   // block #0 grows from the birth (blockchain.md §5.3)
        return branch_origin(storage_.get_node(user_id, leaf_index));
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
            "branch is frozen by revocation — no replacement assigned (blockchain.md §6.7)");
    if (st.state == BranchRevocationState::REPLACED
        && st.next_key.has_value() && signing_pubkey != *st.next_key)
        throw RevocationError(
            "branch key was replaced by revocation; signing key is not the authorized one");
    if (st.awaiting_replacement)
        throw RevocationError(
            "the replacement key starts with its replacement block right after the owner's "
            "line — take_replacement first (blockchain.md §6.7 rule 9)");
    if (st.continuation_off_line)
        throw RevocationError(
            "this branch's replacement block stands off the owner's line in force: it cannot "
            "continue here (blockchain.md §6.7 rule 9)");
}

Block Blockchain::take_replacement(const UserId& user_id, NodeIndex node_index,
                                   const KeyPair& replacement, Timestamp timestamp) {
    const auto st = validator_.branch_revocation_status(user_id, node_index);
    if (!st.awaiting_replacement || !st.next_key.has_value() || *st.next_key != replacement.pub)
        throw RevocationError("take_replacement: no replacement awaits this key in this branch "
                              "(blockchain.md §6.7 rule 9)");
    if (!st.line.has_value())
        throw RevocationError("take_replacement: no line named yet — the replacement starts "
                              "right after the owner's last valid block (blockchain.md §6.7 rule 9)");
    const LastGood& line = *st.line;
    const auto tip = storage_.branch_tip_index(user_id, node_index);
    const Hash end = branch_tip_hash(user_id, node_index);
    const bool at_line = line.index == EMPTY_BRANCH_INDEX
        ? !tip.has_value() && end == line.hash
        : tip.has_value() && *tip == line.index && end == line.hash;
    if (!at_line)
        throw RevocationError("take_replacement: this copy of the branch does not end at the "
                              "owner's line (#" +
                              (line.index == EMPTY_BRANCH_INDEX ? std::string("none")
                                                                : std::to_string(line.index)) +
                              ") — the replacement block stands right after it "
                              "(blockchain.md §6.7 rule 9)");
    const auto record = validator_.replacement_record(user_id, node_index, replacement.pub);
    if (!record.has_value())
        throw RevocationError("take_replacement: the record that assigned this key is not here");

    // Time only goes forward in a branch (blockchain.md §9.4), from its birth.
    Timestamp ts = timestamp;
    if (tip.has_value())
        ts = std::max(ts, storage_.get_block({user_id, node_index, *tip}).timestamp_claimed);
    else if (node_index != 0) {
        const Node         n = storage_.get_node(user_id, node_index);
        const BlockAddress at{user_id, (node_index - 1) / 2, n.birth.index};
        ts = std::max(ts, (storage_.has_block(at) ? storage_.get_block(at)
                                                  : storage_.get_external_block(at)).timestamp_claimed);
    }
    Block k = build_signed_block(
        {user_id, node_index, tip.has_value() ? *tip + 1 : 0}, end, ts, BlockType::KEY_REPLACEMENT,
        Serializer::encode(ReplacementPayload{record->address, Crypto::hash_block(*record)}),
        replacement.sec);
    storage_.put_block(k);
    return k;
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
    // Rotations and replacement blocks alike (blockchain.md §6.6, blockchain.md §6.7 rule 9).
    return validator_.key_at(user_id, node_index, KeyPathCert::kNow);
}

Block Blockchain::revoke_node(
    const UserId&                   user_id,
    NodeIndex                       revoked_node_index,
    const std::optional<PublicKey>& replacement_pubkey,
    NodeIndex                       ancestor_index,
    const KeyPair&                  ancestor_keypair,
    Timestamp                       timestamp,
    const std::optional<Hash>&      last_good)
{
    if (revoked_node_index == 0)
        throw RevocationError("root cannot be revoked — it has no ancestor (blockchain.md §11.5)");
    if (!is_valid_node(revoked_node_index))
        throw InvalidArgumentError("revoke_node: invalid revoked node index");
    // Covers self-revocation: a key cannot revoke itself (blockchain.md §6.7 rule 2).
    if (!is_ancestor(ancestor_index, revoked_node_index))
        throw RevocationError("revocation author is not a strict ancestor of the revoked node");

    // The exact key being revoked: the key of the epoch in force — its lineage
    // tip. A replacement that has not written its block yet is no epoch here:
    // a new record for the same epoch overrides it (rule 3, "the last word"),
    // and the line it names stays this epoch's (rule 9).
    auto target_st = validator_.branch_revocation_status(user_id, revoked_node_index);
    PublicKey revoked_key = target_st.next_key.has_value() && !target_st.awaiting_replacement
        ? *target_st.next_key
        : target_st.current_key;

    // The owner names the line from their own copy of the branch (blockchain.md §6.7
    // rule 12): the last valid block must be one of its blocks.
    std::optional<LastGood> line;
    if (last_good.has_value() &&
        *last_good == branch_origin(storage_.get_node(user_id, revoked_node_index))) {
        line = LastGood{EMPTY_BRANCH_INDEX, *last_good};   // no valid block at all
    } else if (last_good.has_value()) {
        if (auto tip = storage_.branch_tip_index(user_id, revoked_node_index); tip.has_value())
            for (BlockIndex i = 0; i <= *tip && !line.has_value(); ++i)
                if (Crypto::hash_block(storage_.get_block({user_id, revoked_node_index, i}))
                    == *last_good)
                    line = LastGood{i, *last_good};
        if (!line.has_value())
            throw InvalidArgumentError(
                "revoke_node: last_good is not a block of the revoked branch in this storage");
    }
    // A replacement without a line named here keeps the line already in force,
    // so that the record its replacement block names carries it (rule 9).
    if (!line.has_value() && replacement_pubkey.has_value() && target_st.awaiting_replacement)
        line = target_st.line;
    else if (!line.has_value() && replacement_pubkey.has_value() &&
             target_st.state == BranchRevocationState::FROZEN)
        line = target_st.line;

    // The author branch itself must be writable with this keypair (it may have
    // been replaced by its own revocation — then only the replacement signs).
    auto author_st = validator_.branch_revocation_status(user_id, ancestor_index);
    if (author_st.state == BranchRevocationState::FROZEN)
        throw RevocationError("revoke_node: author branch is frozen by revocation");
    if (!author_st.next_key.has_value() || ancestor_keypair.pub != *author_st.next_key)
        throw InvalidArgumentError(
            "revoke_node: keypair does not match the ancestor branch working key");
    if (author_st.awaiting_replacement || author_st.continuation_off_line)
        throw RevocationError("revoke_node: the author's branch has not taken its replacement "
                              "key yet (blockchain.md §6.7 rule 9)");

    Hash prev_hash = branch_tip_hash(user_id, ancestor_index);
    auto tip_opt   = storage_.branch_tip_index(user_id, ancestor_index);
    BlockIndex idx = tip_opt.has_value() ? (*tip_opt + 1) : 0;

    RevocationPayload rp{};
    rp.revoked_node_index = revoked_node_index;
    rp.revoked_pubkey     = revoked_key;
    rp.replacement_pubkey = replacement_pubkey;
    rp.last_good          = line;

    Block b = build_signed_block(
        {user_id, ancestor_index, idx}, prev_hash, timestamp,
        BlockType::REVOCATION, Serializer::encode(rp), ancestor_keypair.sec);
    storage_.put_block(b);
    return b;
}

void Blockchain::restore_branch(const UserId& user_id, NodeIndex node_index,
                                const std::vector<Block>& blocks) {
    validator_.check_continuation(user_id, node_index, blocks);
    for (const Block& b : blocks) storage_.put_block(b);
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
