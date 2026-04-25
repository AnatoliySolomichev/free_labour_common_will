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
