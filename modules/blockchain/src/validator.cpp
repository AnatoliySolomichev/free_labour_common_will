#include "blockchain/validator.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <limits>

namespace blockchain {

// Verify a block signature against a candidate key (signed bytes = canonical
// CBOR with the signature zeroed).
static bool signed_by(const Block& block, const PublicKey& key) {
    Block to_verify = block;
    to_verify.signature = Signature::null();
    auto bytes = Serializer::encode(to_verify);
    return Crypto::verify(bytes.data(), bytes.size(), block.signature, key);
}

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

    // A replacement assigned by revocation (§6.7 rules 3/9) may take over the
    // branch at first use; after the switch the old lineage may not sign.
    auto rev = effective_revocation(user_id, leaf_index);
    bool switched = false;

    for (BlockIndex i = 0; i <= *tip_opt; ++i) {
        Block b = storage_.get_block({user_id, leaf_index, i});

        try {
            validate_block(b, prev_hash, work_key); // checks prev_hash + signature
        } catch (const SignatureError&) {
            if (switched || !rev.has_value() || !rev->replacement_pubkey.has_value())
                throw;
            // §6.7 rule 9: first use of the replacement switches the key.
            validate_block(b, prev_hash, *rev->replacement_pubkey);
            work_key = *rev->replacement_pubkey;
            switched = true;
        }

        if (b.timestamp_claimed < prev_ts)
            throw TimestampError("block timestamp is not monotonically non-decreasing");

        // KEY_ROTATION: payload carries the new working pubkey (32 raw bytes).
        if (b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32)
            std::copy(b.payload.begin(), b.payload.begin() + 32, work_key.bytes.begin());

        prev_hash = Crypto::hash_block(b);
        prev_ts   = b.timestamp_claimed;
    }
}

BranchRevocationStatus Validator::branch_revocation_status(
    const UserId& user_id, NodeIndex node_index) const {
    BranchRevocationStatus out{};
    out.revocation = effective_revocation(user_id, node_index);

    Node node = storage_.get_node(user_id, node_index);
    PublicKey lineage = node.working_pubkey; // pre-replacement rotation lineage
    PublicKey current = lineage;             // authorized key after the switch
    bool switched = false;
    const bool has_repl =
        out.revocation.has_value() && out.revocation->replacement_pubkey.has_value();

    auto tip_opt = storage_.branch_tip_index(user_id, node_index);
    if (tip_opt.has_value()) {
        out.blocks.reserve(*tip_opt + 1);
        for (BlockIndex i = 0; i <= *tip_opt; ++i) {
            Block b = storage_.get_block({user_id, node_index, i});

            if (!switched) {
                if (signed_by(b, lineage)) {
                    const bool suspect = out.revocation.has_value()
                        && b.timestamp_claimed > out.revocation->compromised_since;
                    out.blocks.push_back(suspect ? BlockRevocationStatus::SUSPECT
                                                 : BlockRevocationStatus::CLEAN);
                    if (b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32)
                        std::copy(b.payload.begin(), b.payload.begin() + 32,
                                  lineage.bytes.begin());
                } else if (has_repl && signed_by(b, *out.revocation->replacement_pubkey)) {
                    switched = true;
                    current  = *out.revocation->replacement_pubkey;
                    out.blocks.push_back(BlockRevocationStatus::CLEAN);
                    if (b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32)
                        std::copy(b.payload.begin(), b.payload.begin() + 32,
                                  current.bytes.begin());
                } else {
                    throw SignatureError("block signed by neither the key lineage "
                                         "nor the replacement");
                }
            } else {
                if (signed_by(b, current)) {
                    out.blocks.push_back(BlockRevocationStatus::CLEAN);
                    if (b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32)
                        std::copy(b.payload.begin(), b.payload.begin() + 32,
                                  current.bytes.begin());
                } else if (signed_by(b, lineage)) {
                    // §6.7 rule 9: the old key resurfacing after the switch.
                    out.blocks.push_back(BlockRevocationStatus::INVALID_AFTER_REPLACEMENT);
                    // do not follow its rotations
                } else {
                    throw SignatureError("block signed by neither the replacement "
                                         "lineage nor the old key");
                }
            }
        }
    }

    if (!out.revocation.has_value()) {
        out.state    = BranchRevocationState::ACTIVE;
        out.next_key = lineage;
    } else if (!has_repl) {
        out.state    = BranchRevocationState::FROZEN;
        out.next_key = std::nullopt;
    } else {
        out.state    = BranchRevocationState::REPLACED;
        out.next_key = switched ? current : *out.revocation->replacement_pubkey;
    }
    return out;
}

bool Validator::node_invalidated_by_revocation(
    const UserId& user_id, NodeIndex node_index) const {
    auto indices = path_indices(node_index); // root → node
    for (size_t k = 1; k < indices.size(); ++k) {
        const NodeIndex parent = indices[k - 1];
        const NodeIndex child  = indices[k];
        auto st = effective_revocation(user_id, parent);
        if (!st.has_value()) continue;
        Node c = storage_.get_node(user_id, child);
        // created_at is author-claimed; anti-backdating of node creation is the
        // witnessing-based finalization (§11.2).
        if (c.created_at > st->compromised_since) return true;
    }
    return false;
}

std::vector<std::pair<BlockAddress, RevocationPayload>>
Validator::collect_revocations(const UserId& user_id, NodeIndex target) const {
    std::vector<std::pair<BlockAddress, RevocationPayload>> found;
    if (target == 0) return found;

    for (const BlockAddress& addr : storage_.get_revocation_addresses(user_id, target)) {
        if (!is_ancestor(addr.node_index, target)) continue;

        // Own chains live in `blocks`; partners' revocations arrive as imported
        // certificates and live in `external_blocks` (§6.7 rule 8).
        Block b;
        if      (storage_.has_block(addr))          b = storage_.get_block(addr);
        else if (storage_.has_external_block(addr)) b = storage_.get_external_block(addr);
        else continue;
        if (b.type != BlockType::REVOCATION) continue;
        RevocationPayload rp;
        try {
            rp = Serializer::decode_revocation_payload(b.payload.data(), b.payload.size());
        } catch (const SerializationError&) { continue; }
        if (rp.revoked_node_index != target) continue;

        // §6.7 rule 10: a record from an author that was itself under revocation
        // at writing time does not count — a revocation is a one-sided action and
        // loses weight in the suspect zone. Recursion is finite: authors are
        // strictly shallower, the root is unrevocable. Post-replacement records
        // (signed by the author's replacement key) count; records signed by a
        // rotated successor of the replacement are not recognized here — MVP
        // simplification.
        auto author_state = effective_revocation(user_id, addr.node_index);
        if (author_state.has_value()) {
            const bool by_replacement = author_state->replacement_pubkey.has_value()
                && signed_by(b, *author_state->replacement_pubkey);
            if (!by_replacement && b.timestamp_claimed > author_state->compromised_since)
                continue;
        }

        found.emplace_back(b.address, std::move(rp));
    }

    // Root-most author first, then branch order (§4.4 gradient + "last word").
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) {
                  const uint32_t da = node_depth(a.first.node_index);
                  const uint32_t db = node_depth(b.first.node_index);
                  if (da != db) return da < db;
                  if (a.first.node_index != b.first.node_index)
                      return a.first.node_index < b.first.node_index;
                  return a.first.block_index < b.first.block_index;
              });
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
    for (const auto& [addr, prior] : collect_revocations(uid, rp.revoked_node_index)) {
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
    auto found = collect_revocations(user_id, node_index);
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
