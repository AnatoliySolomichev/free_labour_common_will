#include "blockchain/merge_session.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"

namespace blockchain {

MergeSession::MergeSession(IStorage& storage, Validator& validator)
    : storage_(storage), validator_(validator) {}

// ── Step 1a ───────────────────────────────────────────────────────────────────

BranchTipInfo MergeSession::prepare_tip(const UserId& user_id,
                                         NodeIndex leaf_index) const {
    BranchTipInfo info{};

    for (NodeIndex idx : path_indices(leaf_index))
        info.path.push_back(storage_.get_node(user_id, idx));

    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value()) {
        info.tip_address = {user_id, leaf_index, EMPTY_BRANCH_INDEX};
        info.tip_hash    = Crypto::hash_node(info.path.back());
    } else {
        info.tip_address  = {user_id, leaf_index, *tip_opt};
        Block tip_block   = storage_.get_block(info.tip_address);
        info.tip_hash     = Crypto::hash_block(tip_block);
        info.tip_block    = std::move(tip_block);
    }

    return info;
}

// ── Step 1b ───────────────────────────────────────────────────────────────────

void MergeSession::verify_partner_tip(const BranchTipInfo& partner_tip) const {
    if (partner_tip.path.empty())
        throw ChainIntegrityError("partner BranchTipInfo has empty path");

    // Verify root: self-signed, parent_hash == zero.
    const Node& root = partner_tip.path[0];
    if (root.index != 0)
        throw ChainIntegrityError("first path node must be root (index 0)");
    if (root.parent_hash != Hash::zero())
        throw ChainIntegrityError("partner root node must have zero parent_hash");
    {
        Node to_sign = root;
        to_sign.parent_sig = Signature::null();
        auto b = Serializer::encode(to_sign);
        if (!Crypto::verify(b.data(), b.size(), root.parent_sig, root.structural_pubkey))
            throw SignatureError("partner root node self-signature invalid");
    }

    // Verify subsequent nodes: parent_hash linkage + signature by parent's working key.
    for (size_t i = 1; i < partner_tip.path.size(); ++i) {
        const Node& parent = partner_tip.path[i - 1];
        const Node& child  = partner_tip.path[i];

        if (child.parent_hash != Crypto::hash_node(parent))
            throw ChainIntegrityError("partner path: node parent_hash mismatch");

        Node to_sign = child;
        to_sign.parent_sig = Signature::null();
        auto cb = Serializer::encode(to_sign);
        if (!Crypto::verify(cb.data(), cb.size(), child.parent_sig, parent.working_pubkey))
            throw SignatureError("partner path: node signature invalid");
    }

    // If partner included their tip block, verify its hash matches tip_hash.
    if (partner_tip.tip_block.has_value()) {
        if (Crypto::hash_block(*partner_tip.tip_block) != partner_tip.tip_hash)
            throw ChainIntegrityError("partner tip_block hash does not match tip_hash");
        if (!(partner_tip.tip_block->address == partner_tip.tip_address))
            throw ChainIntegrityError("partner tip_block address does not match tip_address");
    }

    // Local revocation knowledge (§6.7 rule 11 — rules apply when accepting
    // the new): refuse frozen branches, stale tips of replaced branches and
    // fake subtrees. Knowledge = own records + imported certificates; keeping
    // it fresh is the caller's policy (sync.md §10.3).
    validator_.check_tip_against_revocations(partner_tip);
}

// ── Snapshot accessor ─────────────────────────────────────────────────────────

MergeSnapshot MergeSession::snapshot_for(const UserId& user_id,
                                          NodeIndex leaf_index) const {
    if (auto stored = storage_.get_snapshot(user_id, leaf_index))
        return *stored;

    // No merges yet → single-leaf snapshot over the current tip.
    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value())
        throw InvalidArgumentError(
            "snapshot_for: empty branch has no snapshot; append a block first (§6.4)");
    Block tip = storage_.get_block({user_id, leaf_index, *tip_opt});
    ExternalRef ref{{user_id, leaf_index, *tip_opt}, Crypto::hash_block(tip)};
    return MergeSnapshot::leaf(ref);
}

// ── Step 2 ────────────────────────────────────────────────────────────────────

PendingMergeBlock MergeSession::create_pending(
    const UserId&        user_id,
    NodeIndex            leaf_index,
    const BranchTipInfo& partner_tip,
    const MergeSnapshot& partner_snapshot,
    const KeyPair&       own_working_keypair,
    Timestamp            merge_timestamp,
    uint32_t             validated_depth)
{
    // A merge participant must have at least one block (§6.4): both the own branch
    // and the partner branch need a block-0 to appear as snapshot leaves. An empty
    // branch must append a stub block first (§5.4).
    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value())
        throw InvalidArgumentError(
            "cannot merge from an empty branch; append a block first (§6.4)");
    if (partner_tip.tip_address.block_index == EMPTY_BRANCH_INDEX)
        throw InvalidArgumentError(
            "cannot merge with an empty-branch partner (§6.4)");

    // §6.7: a revoked own branch may not open new bilateral acts — frozen at
    // all, replaced only under the authorized key (mirrors
    // Blockchain::ensure_branch_writable; merges bypass the facade).
    if (validator_.effective_revocation(user_id, leaf_index).has_value()) {
        const auto own_st = validator_.branch_revocation_status(user_id, leaf_index);
        if (own_st.state == BranchRevocationState::FROZEN)
            throw RevocationError("own branch is frozen by revocation (§6.7)");
        if (own_st.next_key.has_value() && own_working_keypair.pub != *own_st.next_key)
            throw RevocationError(
                "own branch key was replaced by revocation; use the replacement key (§6.7)");
    }

    // Union own snapshot with the partner's → the packet's new snapshot and the
    // in-block commitments (§6.5.1).
    MergeSnapshot own_snapshot = snapshot_for(user_id, leaf_index);
    MergeSnapshot merged       = MergeSnapshot::merge(own_snapshot, partner_snapshot);

    MergePayload mp{};
    mp.partner_last_address = partner_tip.tip_address;
    mp.partner_last_hash    = partner_tip.tip_hash;
    mp.merge_timestamp      = merge_timestamp;
    mp.merkle_root          = merged.merkle_root;
    mp.hll_hash             = merged.sketch_hash();
    mp.validated_depth      = validated_depth;
    std::vector<uint8_t> payload = Serializer::encode(mp);

    // prev_hash / next index (branch is non-empty).
    Hash prev_hash = Crypto::hash_block(
        storage_.get_block({user_id, leaf_index, *tip_opt}));
    BlockIndex new_idx = *tip_opt + 1;

    Block draft{};
    draft.address           = {user_id, leaf_index, new_idx};
    draft.prev_hash         = prev_hash;
    draft.timestamp_claimed = merge_timestamp;
    draft.type              = BlockType::MERGE;
    draft.payload           = std::move(payload);
    draft.signature         = Signature::null();

    auto bytes      = Serializer::encode(draft);
    draft.signature = Crypto::sign(bytes.data(), bytes.size(), own_working_keypair.sec);

    // Persist the draft and the grown snapshot immediately (mirrors the early draft
    // persistence; both roll back together if a merge is later abandoned).
    storage_.put_block(draft);
    storage_.put_snapshot(user_id, leaf_index, merged);

    return PendingMergeBlock{draft, Crypto::hash_block(draft)};
}

// ── Step 3 ────────────────────────────────────────────────────────────────────

Signature MergeSession::co_sign(const Hash& partner_draft_hash,
                                 const KeyPair& own_working_keypair) {
    return Crypto::sign(partner_draft_hash.bytes.data(),
                        partner_draft_hash.bytes.size(),
                        own_working_keypair.sec);
}

// ── Step 4 ────────────────────────────────────────────────────────────────────

Block MergeSession::finalize(const PendingMergeBlock& pending,
                              const Signature&         partner_co_sig,
                              const PublicKey&         partner_pubkey) {
    if (!Crypto::verify(pending.draft_hash.bytes.data(),
                        pending.draft_hash.bytes.size(),
                        partner_co_sig, partner_pubkey))
        throw SignatureError("partner co_signature is invalid");

    Seal seal{};
    seal.signer_id  = {partner_pubkey.bytes};
    seal.block_hash = pending.draft_hash;
    seal.signature  = partner_co_sig;
    seal.mode       = SealMode::OPEN;
    seal.sealed_at  = pending.draft.timestamp_claimed;
    storage_.put_seal(seal);

    // co_signature is excluded from canonical CBOR encoding (not in LMDB block bytes).
    // The returned block carries it in memory for immediate use by the caller.
    Block finalized        = pending.draft;
    finalized.co_signature = partner_co_sig;
    return finalized;
}

// ── Step 5 (optional) ─────────────────────────────────────────────────────────

void MergeSession::import_partner_data(const BranchTipInfo& partner_tip) {
    if (partner_tip.path.empty()) return;

    const UserId partner_id = partner_tip.path.front().structural_pubkey;

    for (const Node& node : partner_tip.path) {
        if (!storage_.has_node(partner_id, node.index))
            storage_.put_node(partner_id, node);
    }

    if (partner_tip.tip_block.has_value()) {
        const Block& block = *partner_tip.tip_block;
        if (!storage_.has_external_block(block.address))
            storage_.put_external_block(block);
    }
}

} // namespace blockchain
