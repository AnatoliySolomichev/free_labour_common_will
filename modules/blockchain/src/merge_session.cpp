#include "blockchain/merge_session.h"
#include "blockchain/key_path.h"
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
    info.path = KeyPathCert::build(storage_, user_id, leaf_index);

    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value()) {
        info.tip_address = {user_id, leaf_index, EMPTY_BRANCH_INDEX};
        info.tip_hash    = branch_origin(storage_.get_node(user_id, leaf_index));
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
    // Путь ключа партнёра — та же проверка, что у сертификата отзыва
    // (blockchain.md §7.5): корень подписан сам собой, каждое рождение — ключом
    // родителя на своём месте, смены ключа по порядку. Отсюда и право доверять
    // сохранённым узлам: в хранилище попадают только проверенные пути.
    try {
        const UserId chain = KeyPathCert::verify(partner_tip.path);
        if (!(chain == partner_tip.tip_address.user_id) ||
            KeyPathCert::node(partner_tip.path) != partner_tip.tip_address.node_index)
            throw ChainIntegrityError("does not lead to the tip's branch");
    } catch (const ChainIntegrityError& e) {
        throw ChainIntegrityError(std::string("partner path: ") + e.what());
    } catch (const SignatureError& e) {
        throw SignatureError(std::string("partner path: ") + e.what());
    } catch (const SerializationError& e) {
        throw ChainIntegrityError(std::string("partner path: ") + e.what());
    }

    // If partner included their tip block, verify its hash matches tip_hash.
    if (partner_tip.tip_block.has_value()) {
        if (Crypto::hash_block(*partner_tip.tip_block) != partner_tip.tip_hash)
            throw ChainIntegrityError("partner tip_block hash does not match tip_hash");
        if (!(partner_tip.tip_block->address == partner_tip.tip_address))
            throw ChainIntegrityError("partner tip_block address does not match tip_address");
    }

    // Local revocation knowledge (blockchain.md §6.7 rule 11 — rules apply when accepting
    // the new): refuse frozen branches, stale tips of replaced branches and
    // fake subtrees. Knowledge = own records + imported certificates; keeping
    // it fresh is the caller's policy (sync.md §10.3).
    validator_.check_tip_against_revocations(partner_tip);
}

void MergeSession::check_partner_time(const BranchTipInfo& partner_tip, Timestamp now) const {
    if (partner_tip.tip_block && partner_tip.tip_block->timestamp_claimed > now + kClockSlack)
        throw TimestampError("partner tip is dated in the future — merge waits until time "
                             "catches up (blockchain.md §9.4)");
}

void MergeSession::check_partner_half(const Block& half, const BranchTipInfo& own_tip,
                                      const BranchTipInfo& partner_tip, Timestamp now,
                                      const MergeSnapshot& merged) const {
    const BlockAddress& tip = partner_tip.tip_address;
    if (tip.block_index == EMPTY_BRANCH_INDEX)
        throw InvalidArgumentError(
            "cannot merge with an empty-branch partner (blockchain.md §6.4)");
    if (half.type != BlockType::MERGE)
        throw ChainIntegrityError("partner half: not a MERGE block");
    if (!(half.address.user_id == tip.user_id) || half.address.node_index != tip.node_index ||
        half.address.block_index != tip.block_index + 1 || half.prev_hash != partner_tip.tip_hash)
        throw ChainIntegrityError("partner half: not the block right after the partner's tip");
    const MergePayload mp = Serializer::decode_merge_payload(half.payload.data(),
                                                             half.payload.size());
    if (!(mp.partner_last_address == own_tip.tip_address) || mp.partner_last_hash != own_tip.tip_hash)
        throw ChainIntegrityError("partner half: does not take the tip we sent");
    // The time the partner writes is the time we vouch for by answering.
    if (half.timestamp_claimed != mp.merge_timestamp ||
        mp.merge_timestamp > now + kClockSlack || mp.merge_timestamp < now - kClockSlack)
        throw TimestampError("partner half: merge time is not now by our clock "
                             "(blockchain.md §9.4)");
    if (merged.merkle_root != mp.merkle_root || merged.sketch_hash() != mp.hll_hash)
        throw ChainIntegrityError("partner half: commits another union of snapshots");
}

BranchTipInfo MergeSession::tip_after(const BranchTipInfo& tip, const Block& half) {
    BranchTipInfo next = tip;
    next.tip_address   = half.address;
    next.tip_hash      = Crypto::hash_block(half);
    next.tip_block     = half;
    return next;
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
            "snapshot_for: empty branch has no snapshot; append a block first (blockchain.md §6.4)");
    Block tip = storage_.get_block({user_id, leaf_index, *tip_opt});
    ExternalRef ref{{user_id, leaf_index, *tip_opt}, Crypto::hash_block(tip)};
    return MergeSnapshot::leaf(ref);
}

// ── Step 2 ────────────────────────────────────────────────────────────────────

void MergeSession::check_own_branch(const UserId& user_id, NodeIndex leaf_index,
                                    const KeyPair& own_working_keypair) const {
    if (!validator_.effective_revocation(user_id, leaf_index).has_value()) return;
    const auto own_st = validator_.branch_revocation_status(user_id, leaf_index);
    if (own_st.state == BranchRevocationState::FROZEN)
        throw RevocationError("own branch is frozen by revocation (blockchain.md §6.7)");
    if (own_st.next_key.has_value() && own_working_keypair.pub != *own_st.next_key)
        throw RevocationError(
            "own branch key was replaced by revocation; use the replacement key (blockchain.md §6.7)");
    if (own_st.awaiting_replacement)
        throw RevocationError("own branch has not taken its replacement key yet: its replacement "
                              "block comes first (blockchain.md §6.7 rule 9)");
    if (own_st.continuation_off_line)
        throw RevocationError("own branch continues off the owner's line in force "
                              "(blockchain.md §6.7 rule 9)");
}

Block MergeSession::write_half(
    const BranchTipInfo& own_tip,
    const BranchTipInfo& partner_tip,
    const MergeSnapshot& partner_snapshot,
    const KeyPair&       own_working_keypair,
    Timestamp            merge_timestamp,
    uint32_t             validated_depth)
{
    const UserId&   user_id    = own_tip.tip_address.user_id;
    const NodeIndex leaf_index = own_tip.tip_address.node_index;

    // A merge participant must have at least one block (blockchain.md §6.4): both the own branch
    // and the partner branch need a block-0 to appear as snapshot leaves. An empty
    // branch must append a stub block first (blockchain.md §5.4).
    auto tip_opt = storage_.branch_tip_index(user_id, leaf_index);
    if (!tip_opt.has_value())
        throw InvalidArgumentError(
            "cannot merge from an empty branch; append a block first (blockchain.md §6.4)");
    if (partner_tip.tip_address.block_index == EMPTY_BRANCH_INDEX)
        throw InvalidArgumentError(
            "cannot merge with an empty-branch partner (blockchain.md §6.4)");

    // Each half comes right after the tip the other side takes (blockchain.md
    // §6.4): a branch that moved on since then is no longer that tip.
    if (*tip_opt != own_tip.tip_address.block_index)
        throw ChainIntegrityError("own branch moved on during the merge");

    check_own_branch(user_id, leaf_index, own_working_keypair);

    // Union own snapshot with the partner's → the packet's new snapshot and the
    // in-block commitments (blockchain.md §6.5.1).
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

    Block half{};
    half.address           = {user_id, leaf_index, *tip_opt + 1};
    half.prev_hash         = own_tip.tip_hash;
    half.timestamp_claimed = merge_timestamp;
    half.type              = BlockType::MERGE;
    half.payload           = std::move(payload);
    half.signature         = Signature::null();

    auto bytes     = Serializer::encode(half);
    half.signature = Crypto::sign(bytes.data(), bytes.size(), own_working_keypair.sec);

    storage_.put_block(half);
    storage_.put_snapshot(user_id, leaf_index, merged);
    return half;
}

// ── Step 4 (optional) ─────────────────────────────────────────────────────────

void MergeSession::import_partner_data(const BranchTipInfo& partner_tip) {
    const UserId partner_id = partner_tip.path.root.structural_pubkey;

    for (const Node& node : KeyPathCert::nodes(partner_tip.path)) {
        if (!storage_.has_node(partner_id, node.index))
            storage_.put_node(partner_id, node);
    }

    // Births, key changes and the records behind replacements: what a later
    // check of this chain walks (blockchain.md §7.5).
    for (const auto* blocks : {&partner_tip.path.births, &partner_tip.path.changes,
                               &partner_tip.path.records})
        for (const Block& b : *blocks)
            if (!storage_.has_block(b.address) && !storage_.has_external_block(b.address))
                storage_.put_external_block(b);

    if (partner_tip.tip_block.has_value()) {
        const Block& block = *partner_tip.tip_block;
        if (!storage_.has_external_block(block.address))
            storage_.put_external_block(block);
    }
}

} // namespace blockchain
