#include "blockchain/validator.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <limits>

namespace blockchain {

Validator::Validator(const IStorage& storage) : storage_(storage) {}

void Validator::validate_node(const Node& node, const UserId& user_id) const {
    // Signing bytes = canonical CBOR with parent_sig zeroed.
    Node to_sign = node;
    to_sign.parent_sig = Signature::null();
    auto bytes = Serializer::encode(to_sign);

    if (node.index == 0) {
        // Root: self-signed; parent_hash must be zero.
        if (node.parent_hash != Hash::zero())
            throw ChainIntegrityError("root node must have zero parent_hash");
        if (!Crypto::verify(bytes.data(), bytes.size(), node.parent_sig, node.structural_pubkey))
            throw SignatureError("root node self-signature invalid");
    } else {
        NodeIndex parent_idx = (node.index - 1) / 2;
        Node parent = storage_.get_node(user_id, parent_idx); // throws NodeNotFoundError
        Hash expected_ph = Crypto::hash_node(parent);
        if (node.parent_hash != expected_ph)
            throw ChainIntegrityError("node parent_hash does not match stored parent");
        if (!Crypto::verify(bytes.data(), bytes.size(), node.parent_sig, parent.working_pubkey))
            throw SignatureError("node signature by parent key is invalid");
    }
}

void Validator::validate_path(const UserId& user_id, NodeIndex leaf_index) const {
    for (NodeIndex idx : path_indices(leaf_index)) {
        Node n = storage_.get_node(user_id, idx); // throws NodeNotFoundError if absent
        validate_node(n, user_id);
    }
}

void Validator::validate_block(const Block& block,
                               const Hash& expected_prev_hash,
                               const PublicKey& expected_working_pubkey) const {
    if (block.prev_hash != expected_prev_hash)
        throw ChainIntegrityError("block prev_hash does not match expected value");

    // Re-encode with signature zeroed to recover the signed bytes.
    Block to_verify = block;
    to_verify.signature = Signature::null();
    auto bytes = Serializer::encode(to_verify);
    if (!Crypto::verify(bytes.data(), bytes.size(), block.signature, expected_working_pubkey))
        throw SignatureError("block signature invalid");
}

void Validator::validate_branch(const UserId& user_id, NodeIndex leaf_index) const {
    Node leaf = storage_.get_node(user_id, leaf_index);
    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value()) return; // empty branch is valid

    // First block references the hash of the leaf node.
    Hash prev_hash     = Crypto::hash_node(leaf);
    PublicKey work_key = leaf.working_pubkey;
    Timestamp prev_ts  = std::numeric_limits<Timestamp>::min();

    for (BlockIndex i = 0; i <= *tip_opt; ++i) {
        Block b = storage_.get_block({user_id, leaf_index, i});

        validate_block(b, prev_hash, work_key); // checks prev_hash + signature

        if (b.timestamp_claimed < prev_ts)
            throw TimestampError("block timestamp is not monotonically non-decreasing");

        // KEY_ROTATION: payload carries the new working pubkey (32 raw bytes).
        if (b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32)
            std::copy(b.payload.begin(), b.payload.begin() + 32, work_key.bytes.begin());

        prev_hash = Crypto::hash_block(b);
        prev_ts   = b.timestamp_claimed;
    }
}

// All REVOCATION blocks concerning `target` found in the branches of its
// ancestors, in (ancestor depth, block index) order: root-most ancestor first.
static std::vector<std::pair<BlockAddress, RevocationPayload>>
collect_revocations(const IStorage& storage, const UserId& user_id, NodeIndex target) {
    std::vector<std::pair<BlockAddress, RevocationPayload>> found;
    if (target == 0) return found;

    for (NodeIndex anc : path_indices((target - 1) / 2)) {
        if (!storage.has_node(user_id, anc)) continue;
        auto tip_opt = storage.branch_tip_index(user_id, anc);
        if (!tip_opt.has_value()) continue;

        for (BlockIndex i = 0; i <= *tip_opt; ++i) {
            Block b = storage.get_block({user_id, anc, i});
            if (b.type != BlockType::REVOCATION) continue;
            RevocationPayload rp =
                Serializer::decode_revocation_payload(b.payload.data(), b.payload.size());
            if (rp.revoked_node_index == target)
                found.emplace_back(b.address, std::move(rp));
        }
    }
    return found;
}

void Validator::validate_revocation(const Block& block) const {
    if (block.type != BlockType::REVOCATION)
        throw InvalidArgumentError("validate_revocation: block is not a REVOCATION");

    const RevocationPayload rp =
        Serializer::decode_revocation_payload(block.payload.data(), block.payload.size());
    const UserId& uid = block.address.user_id;

    if (rp.revoked_node_index == 0)
        throw RevocationError("root cannot be revoked — it has no ancestor (§11.5)");
    if (!is_valid_node(rp.revoked_node_index))
        throw RevocationError("revoked node index is invalid");
    // Strict ancestry also rules out self-revocation (§6.7 rule 2).
    if (!is_ancestor(block.address.node_index, rp.revoked_node_index))
        throw RevocationError("revocation author is not a strict ancestor of the revoked node");

    // revoked_pubkey must be a historical working key of the revoked branch:
    // the node key, any KEY_ROTATION key, or a replacement assigned by an
    // earlier revocation (stop → replace → revoke-again flow).
    Node revoked = storage_.get_node(uid, rp.revoked_node_index); // NodeNotFoundError
    std::vector<PublicKey> known{revoked.working_pubkey};

    auto tip_opt = storage_.branch_tip_index(uid, rp.revoked_node_index);
    if (tip_opt.has_value()) {
        for (BlockIndex i = 0; i <= *tip_opt; ++i) {
            Block b = storage_.get_block({uid, rp.revoked_node_index, i});
            if (b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32) {
                PublicKey k{};
                std::copy(b.payload.begin(), b.payload.begin() + 32, k.bytes.begin());
                known.push_back(k);
            }
        }
    }
    for (const auto& [addr, prior] : collect_revocations(storage_, uid, rp.revoked_node_index)) {
        if (addr == block.address) continue; // a block cannot legitimize itself
        if (prior.replacement_pubkey.has_value())
            known.push_back(*prior.replacement_pubkey);
    }

    const bool matches = std::any_of(known.begin(), known.end(),
        [&](const PublicKey& k) { return k == rp.revoked_pubkey; });
    if (!matches)
        throw RevocationError("revoked_pubkey does not match any key of the revoked node");
}

std::optional<RevocationState> Validator::effective_revocation(
    const UserId& user_id, NodeIndex node_index) const {
    auto found = collect_revocations(storage_, user_id, node_index);
    if (found.empty()) return std::nullopt;

    // Highest ancestor wins (§4.4): collect_revocations returns ancestors in
    // root→parent order, so the winning branch is the node_index of found[0].
    const NodeIndex winner = found[0].first.node_index;

    RevocationState st{};
    st.compromised_since = std::numeric_limits<Timestamp>::max();
    for (const auto& [addr, rp] : found) {
        if (addr.node_index != winner) continue;
        st.compromised_since   = std::min(st.compromised_since, rp.compromised_since);
        st.replacement_pubkey  = rp.replacement_pubkey; // last block has the last word
        st.latest              = addr;
    }
    return st;
}

void Validator::validate_seal(const Seal& seal) const {
    if (!Crypto::verify(seal.block_hash.bytes.data(), seal.block_hash.bytes.size(),
                        seal.signature, seal.signer_id))
        throw SignatureError("seal signature invalid");
}

void Validator::validate_co_signature(const Block& block,
                                      const PublicKey& partner_pubkey) const {
    if (block.type != BlockType::MERGE)
        throw InvalidArgumentError("co_signature validation only applies to MERGE blocks");
    if (!block.co_signature.has_value())
        throw SignatureError("MERGE block has no co_signature");
    Hash bh = Crypto::hash_block(block);
    if (!Crypto::verify(bh.bytes.data(), bh.bytes.size(), *block.co_signature, partner_pubkey))
        throw SignatureError("MERGE block co_signature is invalid");
}

} // namespace blockchain
