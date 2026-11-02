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
    if (node.index == 0) {
        // The root: its own record, signed by itself (blockchain.md §6.1).
        if (!(node.birth == BirthRef{}))
            throw ChainIntegrityError("root node names a birth");
        Node to_sign = node;
        to_sign.root_sig = Signature::null();
        const auto bytes = Serializer::encode(to_sign);
        if (!Crypto::verify(bytes.data(), bytes.size(), node.root_sig, node.structural_pubkey))
            throw SignatureError("root node self-signature invalid");
        return;
    }
    // Any other node is what its birth block says, and the birth is signed by
    // the parent's key in force at its place (blockchain.md §11.2).
    const NodeIndex    parent = node.parent_index();
    const BlockAddress at{user_id, parent, node.birth.index};
    if (!storage_.has_node(user_id, parent)) throw NodeNotFoundError(parent);
    const Block birth = storage_.has_block(at) ? storage_.get_block(at)
                                               : storage_.get_external_block(at);
    if (Crypto::hash_block(birth) != node.birth.hash)
        throw ChainIntegrityError("node: its birth block is not the one it names");
    if (!(node_born(birth) == node))
        throw ChainIntegrityError("node: differs from what its birth block says");
    if (!block_signed_by(birth, key_at(user_id, parent, node.birth.index)))
        throw SignatureError("node: birth is not signed by the parent's key at its place");
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

    // Block #0 grows from the node's birth (the root's own record) (blockchain.md §5.3).
    Hash prev_hash    = branch_origin(leaf);
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

// The line a replacement continues after (blockchain.md §6.7 rules 9, 12): the
// latest one named for the epoch it replaces — or for a replacement on the way
// to it that was revoked before any use.
std::optional<LastGood> line_before(const Records& records, const std::vector<PublicKey>& epoch,
                                    const std::optional<PublicKey>& next) {
    EpochRevocation er = epoch_revocation(records, epoch);
    std::optional<LastGood> line = er.last_good;
    std::vector<PublicKey>  passed;
    while (er.replacement.has_value() && !(next.has_value() && *er.replacement == *next) &&
           !has_key(passed, *er.replacement)) {
        passed.push_back(*er.replacement);
        er = epoch_revocation(records, {*er.replacement});
        if (!er.any) break;
        if (er.last_good.has_value()) line = er.last_good;
    }
    return line;
}

// Whether a KEY_REPLACEMENT block names a counting record that assigned `key`
// (blockchain.md §6.7 rule 9): address and hash of the REVOCATION block.
bool names_record(const IStorage& storage, const Records& records, const Block& k,
                  const PublicKey& key) {
    ReplacementPayload p{};
    try {
        p = Serializer::decode_replacement_payload(k.payload.data(), k.payload.size());
    } catch (const SerializationError&) { return false; }
    for (const auto& [addr, rp] : records) {
        if (!(addr == p.record_address) || !rp.replacement_pubkey || *rp.replacement_pubkey != key)
            continue;
        const bool own = storage.has_block(addr);
        if (!own && !storage.has_external_block(addr)) continue;
        const Block rec = own ? storage.get_block(addr) : storage.get_external_block(addr);
        if (Crypto::hash_block(rec) == p.record_hash) return true;
    }
    return false;
}

// Pass 1 of the branch walk (blockchain.md §6.7 rules 3, 9): every block of the
// branch, stored and then `more`, up to `upto`, attributed to a key epoch — the
// node key with its rotations, then each replacement from its replacement
// block: a KEY_REPLACEMENT signed by the replacement and naming the record that
// assigned it (решение участника 2026-11-02).
struct Seen { size_t epoch; bool old_key; Timestamp ts; Hash hash; Hash prev; };

struct Attribution {
    std::vector<std::vector<PublicKey>> epochs;
    std::vector<BlockIndex>             starts;    // where each epoch begins; 0 for the node's
    std::vector<PublicKey>              retired;   // keys of the epochs left behind
    PublicKey                           current{}; // lineage tip of the current epoch
    std::vector<Seen>                   seen;
};

Attribution attribute(const IStorage& storage, const UserId& user_id, NodeIndex node_index,
                      const Records& records, const std::vector<Block>& more, BlockIndex upto) {
    Attribution a;
    const Node node = storage.get_node(user_id, node_index);
    a.epochs  = {{node.working_pubkey}};
    a.starts  = {0};
    a.current = node.working_pubkey;

    const auto       tip_opt = storage.branch_tip_index(user_id, node_index);
    const BlockIndex stored  = tip_opt.has_value() ? *tip_opt + 1 : 0;
    const BlockIndex total   = std::min<BlockIndex>(stored + static_cast<BlockIndex>(more.size()),
                                                    upto);
    a.seen.reserve(total);
    for (BlockIndex i = 0; i < total; ++i) {
        const Block b = i < stored ? storage.get_block({user_id, node_index, i})
                                   : more[i - stored];
        bool old_key = false;
        if (!block_signed_by(b, a.current)) {
            // A replacement takes over at its replacement block (rule 9) —
            // possibly over replacements revoked before any use.
            const auto next = b.type == BlockType::KEY_REPLACEMENT
                ? follow_replacements(records, epoch_revocation(records, a.epochs.back()),
                                      [&](const PublicKey& k) { return block_signed_by(b, k); })
                : std::nullopt;
            if (next.has_value() && block_signed_by(b, *next) &&
                names_record(storage, records, b, *next)) {
                a.retired.insert(a.retired.end(), a.epochs.back().begin(), a.epochs.back().end());
                a.epochs.push_back({*next});
                a.starts.push_back(i);
                a.current = *next;
            } else if (std::any_of(a.retired.begin(), a.retired.end(),
                           [&](const PublicKey& k) { return block_signed_by(b, k); })) {
                old_key = true; // an old key resurfacing after the switch
            } else {
                throw SignatureError("block signed by none of the branch keys (lineage, "
                                     "replacements — a replacement starts with its "
                                     "KEY_REPLACEMENT block, blockchain.md §6.7 rule 9)");
            }
        }
        if (!old_key && b.type == BlockType::KEY_ROTATION && b.payload.size() >= 32) {
            std::copy(b.payload.begin(), b.payload.begin() + 32, a.current.bytes.begin());
            a.epochs.back().push_back(a.current);
        }
        a.seen.push_back({a.epochs.size() - 1, old_key, b.timestamp_claimed,
                          Crypto::hash_block(b), b.prev_hash});
    }
    return a;
}

} // namespace

BranchRevocationStatus Validator::branch_revocation_status(
    const UserId& user_id, NodeIndex node_index) const {
    return status_over(user_id, node_index, {});
}

std::optional<Block> Validator::replacement_record(const UserId& user_id, NodeIndex node_index,
                                                   const PublicKey& key) const {
    const auto records = winning_records(user_id, node_index);
    for (auto it = records.rbegin(); it != records.rend(); ++it) {
        const auto& [addr, rp] = *it;
        if (!rp.replacement_pubkey || *rp.replacement_pubkey != key) continue;
        if (storage_.has_block(addr))          return storage_.get_block(addr);
        if (storage_.has_external_block(addr)) return storage_.get_external_block(addr);
    }
    return std::nullopt;
}

PublicKey Validator::key_at(const UserId& user_id, NodeIndex node_index, BlockIndex place) const {
    return attribute(storage_, user_id, node_index, winning_records(user_id, node_index), {},
                     place).current;
}

void Validator::check_continuation(const UserId& user_id, NodeIndex node_index,
                                   const std::vector<Block>& more) const {
    if (more.empty()) return;
    const Node node = storage_.get_node(user_id, node_index);
    const auto tip  = storage_.branch_tip_index(user_id, node_index);
    BlockIndex next    = tip.has_value() ? *tip + 1 : 0;
    Hash       prev    = branch_origin(node);
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
    const Attribution att = attribute(storage_, user_id, node_index, records, more,
                                      std::numeric_limits<BlockIndex>::max());
    const auto& epochs  = att.epochs;
    const auto& seen    = att.seen;
    const PublicKey& current = att.current;

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
    // A replacement stands only from its block right after the line of the
    // epoch it replaces (rule 9); a block elsewhere — the line moved, or none
    // was named — takes its epoch and every later one off the owner's line.
    std::size_t off_from = epochs.size();
    for (std::size_t e = 1; e < epochs.size() && off_from == epochs.size(); ++e) {
        const auto       line = line_before(records, epochs[e - 1], epochs[e].front());
        const BlockIndex at   = att.starts[e];
        const bool placed = line.has_value() && seen[at].prev == line->hash &&
            (line->index == EMPTY_BRANCH_INDEX ? at == 0 : at == line->index + 1);
        if (!placed) off_from = e;
    }
    out.continuation_off_line = off_from < epochs.size();
    out.blocks.reserve(seen.size());
    for (BlockIndex i = 0; i < seen.size(); ++i) {
        const Seen&            s  = seen[i];
        const EpochRevocation& er = revs[s.epoch];
        if (s.old_key)
            out.blocks.push_back(BlockRevocationStatus::INVALID_AFTER_REPLACEMENT);
        else if (s.epoch >= off_from)
            out.blocks.push_back(BlockRevocationStatus::OUTSIDE_LINE);
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
        // The epoch in force is revoked: its replacement, if any, has not
        // written its block here yet, and starts right after this line.
        out.awaiting_replacement = out.next_key.has_value();
        out.line                 = line_before(records, epochs.back(), out.next_key);
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
    const UserId chain = tip.path.root.structural_pubkey;
    const auto   nodes = KeyPathCert::nodes(tip.path);

    // Fake subtrees (blockchain.md §6.7 rule 6): an edge created under a revoked parent key
    // after its compromise poisons everything below. Nodes come from the tip
    // itself — the partner's tree need not be in storage.
    for (size_t k = 1; k < nodes.size(); ++k) {
        auto st = effective_revocation(chain, nodes[k - 1].index);
        if (st.has_value() && !birth_stands(tip.path.births[k - 1], *st))
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
