#include "blockchain/key_path.h"
#include "blockchain/validator.h"
#include "blockchain/crypto.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <algorithm>
#include <limits>

namespace blockchain {

bool block_signed_by(const Block& block, const PublicKey& key) {
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
        const BlockAddress at{user_id, parent_idx, node.birth.index};
        check_birth(storage_.has_block(at) ? storage_.get_block(at) : storage_.get_external_block(at),
                    node, parent, user_id);
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

    // Signatures follow the key epochs: the lineage, then each replacement from
    // its first use (blockchain.md §6.7 rules 3/9) — the walk attributes every
    // block or throws SignatureError.
    if (leaf_index != 0) {
        // No branch is older than its birth (blockchain.md §11.2).
        const BlockAddress at{user_id, (leaf_index - 1) / 2, leaf.birth.index};
        const Block birth = storage_.has_block(at) ? storage_.get_block(at)
                                                   : storage_.get_external_block(at);
        if (storage_.get_block({user_id, leaf_index, 0}).timestamp_claimed < birth.timestamp_claimed)
            throw TimestampError("branch begins before its birth (blockchain.md §11.2)");
    }
    const auto st = branch_revocation_status(user_id, leaf_index);

    // First block references the hash of the leaf node.
    Hash prev_hash    = Crypto::hash_node(leaf);
    Timestamp prev_ts = std::numeric_limits<Timestamp>::min();

    for (BlockIndex i = 0; i <= *tip_opt; ++i) {
        Block b = storage_.get_block({user_id, leaf_index, i});

        if (b.prev_hash != prev_hash)
            throw ChainIntegrityError("block prev_hash does not match expected value");
        if (st.blocks[i] == BlockRevocationStatus::INVALID_AFTER_REPLACEMENT)
            throw SignatureError("block signed by a key the replacement took over from "
                                 "(blockchain.md §6.7 rule 9)");
        if (b.timestamp_claimed < prev_ts)
            throw TimestampError("block timestamp is not monotonically non-decreasing");

        prev_hash = Crypto::hash_block(b);
        prev_ts   = b.timestamp_claimed;
    }
}

namespace {

using Records = std::vector<std::pair<BlockAddress, RevocationPayload>>;

bool has_key(const std::vector<PublicKey>& keys, const PublicKey& k) {
    return std::find(keys.begin(), keys.end(), k) != keys.end();
}

// The revocation of one key epoch, merged over the records that name any key
// of the epoch (blockchain.md §6.7 rules 3, 12): the latest word on the replacement,
// the latest named last valid block.
struct EpochRevocation {
    bool                     any   = false;
    std::optional<PublicKey> replacement;
    std::optional<LastGood>  last_good;
};

EpochRevocation epoch_revocation(const Records& records, const std::vector<PublicKey>& keys) {
    EpochRevocation er;
    for (const auto& [addr, rp] : records) {
        (void)addr;
        if (!has_key(keys, rp.revoked_pubkey)) continue;
        er.any         = true;
        er.replacement = rp.replacement_pubkey; // the last word
        if (rp.last_good.has_value()) er.last_good = rp.last_good;
    }
    return er;
}

// Where the replacement chain of a revoked epoch leads: its replacement, or —
// when that one was revoked in turn before any use — that one's replacement,
// and so on. `accepts` picks the first key on the chain it agrees to; with an
// always-false `accepts` the chain is followed to its end. nullopt — the chain
// ends frozen (a revoked key without a replacement) or found nothing.
template <typename Accepts>
std::optional<PublicKey> follow_replacements(const Records& records,
                                             const EpochRevocation& from,
                                             Accepts accepts) {
    if (!from.any || !from.replacement.has_value()) return std::nullopt;
    std::vector<PublicKey> passed;
    PublicKey cand = *from.replacement;
    for (;;) {
        if (accepts(cand)) return cand;
        const EpochRevocation next = epoch_revocation(records, {cand});
        if (!next.any) return cand;                  // the chain's live end
        passed.push_back(cand);
        if (!next.replacement.has_value() || has_key(passed, *next.replacement))
            return std::nullopt;                     // frozen (or a cycle)
        cand = *next.replacement;
    }
}

} // namespace

BranchRevocationStatus Validator::branch_revocation_status(
    const UserId& user_id, NodeIndex node_index) const {
    return status_over(user_id, node_index, {});
}

void Validator::check_continuation(const UserId& user_id, NodeIndex node_index,
                                   const std::vector<Block>& more) const {
    if (more.empty()) return;
    const Node node = storage_.get_node(user_id, node_index);
    const auto tip  = storage_.branch_tip_index(user_id, node_index);
    BlockIndex next    = tip.has_value() ? *tip + 1 : 0;
    Hash       prev    = Crypto::hash_node(node);
    Timestamp  prev_ts = std::numeric_limits<Timestamp>::min();
    if (tip.has_value()) {
        const Block last = storage_.get_block({user_id, node_index, *tip});
        prev    = Crypto::hash_block(last);
        prev_ts = last.timestamp_claimed;
    } else if (node_index != 0) {
        // No branch is older than its birth (blockchain.md §11.2).
        const BlockAddress at{user_id, (node_index - 1) / 2, node.birth.index};
        prev_ts = (storage_.has_block(at) ? storage_.get_block(at)
                                          : storage_.get_external_block(at)).timestamp_claimed;
    }
    for (const Block& b : more) {
        if (!(b.address.user_id == user_id) || b.address.node_index != node_index ||
            b.address.block_index != next)
            throw ChainIntegrityError("block #" + std::to_string(b.address.block_index) +
                                      " does not continue the branch: expected #" +
                                      std::to_string(next) + " of node " +
                                      std::to_string(node_index));
        if (b.prev_hash != prev)
            throw ChainIntegrityError("block #" + std::to_string(next) +
                                      " does not continue the branch: prev_hash");
        if (b.timestamp_claimed < prev_ts)
            throw TimestampError(next == 0
                ? "branch begins before its birth (blockchain.md §11.2)"
                : "block timestamp is not monotonically non-decreasing");
        prev    = Crypto::hash_block(b);
        prev_ts = b.timestamp_claimed;
        ++next;
    }
    // Signatures follow the key epochs (rules 3, 9), as in validate_branch: the
    // walk attributes every block or throws SignatureError.
    const auto st = status_over(user_id, node_index, more);
    for (BlockIndex i = next - static_cast<BlockIndex>(more.size()); i < next; ++i)
        if (st.blocks[i] == BlockRevocationStatus::INVALID_AFTER_REPLACEMENT)
            throw SignatureError("block #" + std::to_string(i) + " signed by a key the "
                                 "replacement took over from (blockchain.md §6.7 rule 9)");
}

BranchRevocationStatus Validator::status_over(const UserId& user_id, NodeIndex node_index,
                                              const std::vector<Block>& more) const {
    BranchRevocationStatus out{};
    const Records records = winning_records(user_id, node_index);
    out.revocation = effective_revocation(user_id, node_index);

    // Pass 1: attribute every block to a key epoch (blockchain.md §6.7 rules 3, 9).
    Node node = storage_.get_node(user_id, node_index);
    std::vector<std::vector<PublicKey>> epochs{{node.working_pubkey}};
    std::vector<PublicKey> retired;          // keys of the epochs left behind
    PublicKey current = node.working_pubkey; // lineage tip of the current epoch

    struct Seen { size_t epoch; bool old_key; Timestamp ts; Hash hash; };
    std::vector<Seen> seen;

    const auto       tip_opt = storage_.branch_tip_index(user_id, node_index);
    const BlockIndex stored  = tip_opt.has_value() ? *tip_opt + 1 : 0;
    const BlockIndex total   = stored + static_cast<BlockIndex>(more.size());
    {
        seen.reserve(total);
        for (BlockIndex i = 0; i < total; ++i) {
            const Block b = i < stored ? storage_.get_block({user_id, node_index, i})
                                       : more[i - stored];
            bool old_key = false;
            if (!block_signed_by(b, current)) {
                // The first use of a replacement switches the key (rule 9) —
                // possibly over replacements revoked before any use.
                const auto next = follow_replacements(
                    records, epoch_revocation(records, epochs.back()),
                    [&](const PublicKey& k) { return block_signed_by(b, k); });
                if (next.has_value() && block_signed_by(b, *next)) {
                    retired.insert(retired.end(), epochs.back().begin(), epochs.back().end());
                    epochs.push_back({*next});
                    current = *next;
                } else if (std::any_of(retired.begin(), retired.end(),
                               [&](const PublicKey& k) { return block_signed_by(b, k); })) {
                    old_key = true; // an old key resurfacing after the switch
                } else {
                    throw SignatureError("block signed by none of the branch keys "
                                         "(lineage, replacements)");
                }
            }
            if (!old_key && b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32) {
                std::copy(b.payload.begin(), b.payload.begin() + 32, current.bytes.begin());
                epochs.back().push_back(current);
            }
            seen.push_back({epochs.size() - 1, old_key, b.timestamp_claimed,
                            Crypto::hash_block(b)});
        }
    }

    // Pass 2: classify. A revoked epoch splits by the named line (rule 12); with
    // no line named yet its blocks stay in question. Blocks past what is here
    // do not matter: the number decides.
    std::vector<EpochRevocation> revs;
    for (const auto& keys : epochs) {
        revs.push_back(epoch_revocation(records, keys));
        const auto& lg = revs.back().last_good;
        if (revs.back().any && lg.has_value() && lg->index != EMPTY_BRANCH_INDEX)
            out.line_end = lg->index;
    }
    out.blocks.reserve(seen.size());
    for (BlockIndex i = 0; i < seen.size(); ++i) {
        const Seen&            s  = seen[i];
        const EpochRevocation& er = revs[s.epoch];
        if (s.old_key)
            out.blocks.push_back(BlockRevocationStatus::INVALID_AFTER_REPLACEMENT);
        else if (!er.any)
            out.blocks.push_back(BlockRevocationStatus::CLEAN);
        else if (!er.last_good.has_value())
            out.blocks.push_back(BlockRevocationStatus::SUSPECT);
        else {
            const LastGood& lg = *er.last_good;
            const bool on_line = lg.index != EMPTY_BRANCH_INDEX &&
                (i < lg.index || (i == lg.index && s.hash == lg.hash));
            out.blocks.push_back(on_line ? BlockRevocationStatus::CLEAN
                                         : BlockRevocationStatus::OUTSIDE_LINE);
        }
    }

    out.current_key = current;
    if (!out.revocation.has_value()) {
        out.state    = BranchRevocationState::ACTIVE;
        out.next_key = current;
    } else if (!revs.back().any) {
        // The epoch in force was never revoked: a replacement took over, or the
        // records name keys this branch has not shown yet.
        out.state    = epochs.size() > 1 ? BranchRevocationState::REPLACED
                                         : BranchRevocationState::ACTIVE;
        out.next_key = current;
    } else {
        out.next_key = follow_replacements(records, revs.back(),
                                           [](const PublicKey&) { return false; });
        out.state    = out.next_key.has_value() ? BranchRevocationState::REPLACED
                                                : BranchRevocationState::FROZEN;
    }
    return out;
}

bool Validator::birth_stands(const Block& birth, const RevocationState& st) const {
    if (!st.last_good.has_value()) return false;            // stopped: no line yet
    const LastGood& line = *st.last_good;
    if (line.index == EMPTY_BRANCH_INDEX || birth.address.block_index > line.index) return false;
    const Hash mine = Crypto::hash_block(birth);
    if (birth.address.block_index == line.index) return mine == line.hash;
    // Along the owner's line, as far as its blocks are here; past that, by the number.
    Hash at = line.hash;
    for (BlockIndex i = line.index; i > birth.address.block_index; --i) {
        const BlockAddress step{birth.address.user_id, birth.address.node_index, i};
        Block b;
        if      (storage_.has_block(step))          b = storage_.get_block(step);
        else if (storage_.has_external_block(step)) b = storage_.get_external_block(step);
        else return true;
        if (Crypto::hash_block(b) != at) return true;   // not the line's block: nothing to walk
        at = b.prev_hash;
    }
    return at == mine;
}

bool Validator::node_invalidated_by_revocation(
    const UserId& user_id, NodeIndex node_index) const {
    auto indices = path_indices(node_index); // root → node
    for (size_t k = 1; k < indices.size(); ++k) {
        const NodeIndex parent = indices[k - 1];
        auto st = effective_revocation(user_id, parent);
        if (!st.has_value()) continue;
        const Node         c = storage_.get_node(user_id, indices[k]);
        const BlockAddress at{user_id, parent, c.birth.index};
        if (!birth_stands(storage_.has_block(at) ? storage_.get_block(at)
                                                 : storage_.get_external_block(at), *st))
            return true;
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
        // certificates and live in `external_blocks` (blockchain.md §6.7 rule 8).
        Block b;
        const bool own = storage_.has_block(addr);
        if      (own)                               b = storage_.get_block(addr);
        else if (storage_.has_external_block(addr)) b = storage_.get_external_block(addr);
        else continue;
        if (b.type != BlockType::REVOCATION) continue;
        RevocationPayload rp;
        try {
            rp = Serializer::decode_revocation_payload(b.payload.data(), b.payload.size());
        } catch (const SerializationError&) { continue; }
        if (rp.revoked_node_index != target) continue;

        // blockchain.md §6.7 rule 10: a record from an author that was itself under revocation
        // at writing time does not count — a revocation is a one-sided action and
        // loses weight in the suspect zone, or off the author's line (rule 12).
        // Recursion is finite: authors are strictly shallower, the root is
        // unrevocable.
        auto author_state = effective_revocation(user_id, addr.node_index);
        if (author_state.has_value() && !author_record_counts(b, own, *author_state))
            continue;

        found.emplace_back(b.address, std::move(rp));
    }

    // Root-most author first, then branch order (blockchain.md §4.4 gradient + "last word").
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
        throw RevocationError("root cannot be revoked — it has no ancestor (blockchain.md §11.5)");
    if (!is_valid_node(rp.revoked_node_index))
        throw RevocationError("revoked node index is invalid");
    // Strict ancestry also rules out self-revocation (blockchain.md §6.7 rule 2).
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

bool Validator::author_record_counts(const Block& record, bool own_branch,
                                     const RevocationState& author_state) const {
    if (own_branch) {
        // The author's branch is ours: its walk places the record on the
        // owner's side of the author's own revocation, or not.
        try {
            const auto st = branch_revocation_status(record.address.user_id,
                                                     record.address.node_index);
            return record.address.block_index < st.blocks.size()
                && st.blocks[record.address.block_index] == BlockRevocationStatus::CLEAN;
        } catch (const std::exception&) {
            // a broken author branch — judge by the line below
        }
    }
    // An imported certificate comes without the author's branch: by the number
    // against the named line, or signed by the author's replacement (a rotated successor of the
    // replacement is not recognized here — MVP simplification).
    const bool by_replacement = author_state.replacement_pubkey.has_value()
        && block_signed_by(record, *author_state.replacement_pubkey);
    const auto& line = author_state.last_good;
    return by_replacement || (line.has_value() && line->index != EMPTY_BRANCH_INDEX &&
                              record.address.block_index <= line->index);
}

std::vector<std::pair<BlockAddress, RevocationPayload>>
Validator::winning_records(const UserId& user_id, NodeIndex target) const {
    auto found = collect_revocations(user_id, target);
    if (found.empty()) return found;

    // Highest ancestor wins (blockchain.md §4.4): collect_revocations returns ancestors in
    // root→parent order, so the winning branch is the node_index of found[0].
    const NodeIndex winner = found[0].first.node_index;
    found.erase(std::remove_if(found.begin(), found.end(),
                               [&](const auto& r) { return r.first.node_index != winner; }),
                found.end());
    return found;
}

std::optional<RevocationState> Validator::effective_revocation(
    const UserId& user_id, NodeIndex node_index) const {
    const auto found = winning_records(user_id, node_index);
    if (found.empty()) return std::nullopt;

    RevocationState st{};
    for (const auto& [addr, rp] : found) {
        st.replacement_pubkey  = rp.replacement_pubkey; // last block has the last word
        if (rp.last_good.has_value()) st.last_good = rp.last_good;
        st.latest              = addr;
    }
    return st;
}

void Validator::check_tip_against_revocations(const BranchTipInfo& tip) const {
    if (tip.path.empty()) return;
    const UserId chain = tip.path.front().structural_pubkey;

    // Fake subtrees (blockchain.md §6.7 rule 6): an edge created under a revoked parent key
    // after its compromise poisons everything below. Nodes come from the tip
    // itself — the partner's tree need not be in storage.
    for (size_t k = 1; k < tip.path.size(); ++k) {
        auto st = effective_revocation(chain, tip.path[k - 1].index);
        if (st.has_value() && k - 1 < tip.births.size() && !birth_stands(tip.births[k - 1], *st))
            throw RevocationError(
                "partner path node was created under a revoked key (blockchain.md §6.7 rule 6)");
    }

    // The branch itself (blockchain.md §6.7 rules 3/9): frozen → no new bilateral acts;
    // replaced → the tip must already run under the replacement key.
    auto st = effective_revocation(chain, tip.tip_address.node_index);
    if (!st.has_value()) return;
    if (!st->replacement_pubkey.has_value())
        throw RevocationError("partner branch is frozen by revocation (blockchain.md §6.7)");
    if (tip.tip_block.has_value() &&
        !block_signed_by(*tip.tip_block, *st->replacement_pubkey))
        throw RevocationError(
            "partner tip is not signed by the revocation replacement key (blockchain.md §6.7)");
}

} // namespace blockchain
