#include "sync/merge_dialogue.h"

#include <blockchain/errors.h>
#include <blockchain/merkle.h>
#include <blockchain/serializer.h>

namespace chainsync {

using blockchain::Block;
using blockchain::BranchTipInfo;
using blockchain::ExternalRef;
using blockchain::Hash;
using blockchain::MergeSnapshot;
using blockchain::MerkleTree;
using blockchain::PublicKey;
using blockchain::SerializationError;
using blockchain::Serializer;
using blockchain::Signature;

// ── Wire format ───────────────────────────────────────────────────────────────
//
// Every message is a deterministic-CBOR (RFC 8949 §4.2.1) envelope:
//
//   [ uint version, uint type, bstr payload ]
//
//   OFFER / ACCEPT payload: [ bstr encode(BranchTipInfo), bstr encode(MergeSnapshot) ]
//   DRAFT payload:          the 32-byte draft hash
//   COSIG payload:          the 64-byte co-signature
//
// Decoding is strict: exact array sizes, minimal-length heads, no trailing
// bytes. Anything else is a protocol failure.

namespace {

constexpr uint64_t WIRE_VERSION = 1;

enum class MsgType : uint8_t { OFFER = 1, ACCEPT = 2, DRAFT = 3, COSIG = 4 };

void put_head(std::vector<uint8_t>& out, uint8_t major, uint64_t v) {
    const uint8_t m = static_cast<uint8_t>(major << 5);
    if (v < 24) {
        out.push_back(static_cast<uint8_t>(m | v));
        return;
    }
    int extra = v <= 0xFF ? 1 : v <= 0xFFFF ? 2 : v <= 0xFFFF'FFFFull ? 4 : 8;
    out.push_back(static_cast<uint8_t>(m | (extra == 1 ? 24 : extra == 2 ? 25
                                          : extra == 4 ? 26 : 27)));
    for (int i = extra - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void put_bstr(std::vector<uint8_t>& out, const std::vector<uint8_t>& bytes) {
    put_head(out, 2, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

struct CborReader {
    const uint8_t* data;
    size_t         len;
    size_t         pos = 0;

    uint8_t byte() {
        if (pos >= len) throw SerializationError("merge message: truncated");
        return data[pos++];
    }

    uint64_t head(uint8_t expected_major) {
        const uint8_t b = byte();
        if ((b >> 5) != expected_major)
            throw SerializationError("merge message: unexpected CBOR type");
        const uint8_t ai = b & 0x1F;
        if (ai < 24) return ai;
        if (ai > 27) throw SerializationError("merge message: bad CBOR head");
        const int extra = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : 8;
        uint64_t v = 0;
        for (int i = 0; i < extra; ++i) v = (v << 8) | byte();
        // Deterministic encoding: reject non-minimal heads.
        static constexpr uint64_t min_for[] = {24, 0x100, 0x1'0000, 0x1'0000'0000ull};
        if (v < min_for[ai - 24])
            throw SerializationError("merge message: non-minimal CBOR head");
        return v;
    }

    std::vector<uint8_t> bstr() {
        const uint64_t n = head(2);
        if (n > len - pos) throw SerializationError("merge message: truncated bstr");
        std::vector<uint8_t> out(data + pos, data + pos + n);
        pos += n;
        return out;
    }

    void expect_end() const {
        if (pos != len) throw SerializationError("merge message: trailing bytes");
    }
};

std::vector<uint8_t> encode_envelope(MsgType type, std::vector<uint8_t> payload) {
    std::vector<uint8_t> out;
    put_head(out, 4, 3);   // array(3)
    put_head(out, 0, WIRE_VERSION);
    put_head(out, 0, static_cast<uint64_t>(type));
    put_bstr(out, payload);
    return out;
}

struct Envelope {
    MsgType              type;
    std::vector<uint8_t> payload;
};

Envelope decode_envelope(const uint8_t* data, size_t len) {
    CborReader r{data, len};
    if (r.head(4) != 3) throw SerializationError("merge message: bad envelope");
    if (r.head(0) != WIRE_VERSION)
        throw SerializationError("merge message: unsupported version");
    const uint64_t t = r.head(0);
    if (t < 1 || t > 4) throw SerializationError("merge message: unknown type");
    Envelope env{static_cast<MsgType>(t), r.bstr()};
    r.expect_end();
    return env;
}

std::vector<uint8_t> encode_tip_snapshot(MsgType type, const BranchTipInfo& tip,
                                         const MergeSnapshot& snapshot) {
    std::vector<uint8_t> payload;
    put_head(payload, 4, 2);   // array(2)
    put_bstr(payload, Serializer::encode(tip));
    put_bstr(payload, Serializer::encode(snapshot));
    return encode_envelope(type, std::move(payload));
}

void decode_tip_snapshot(const std::vector<uint8_t>& payload,
                         BranchTipInfo& tip, MergeSnapshot& snapshot) {
    CborReader r{payload.data(), payload.size()};
    if (r.head(4) != 2) throw SerializationError("merge message: bad tip payload");
    const std::vector<uint8_t> tip_bytes  = r.bstr();
    const std::vector<uint8_t> snap_bytes = r.bstr();
    r.expect_end();
    tip      = Serializer::decode_tip(tip_bytes.data(), tip_bytes.size());
    snapshot = Serializer::decode_snapshot(snap_bytes.data(), snap_bytes.size());
}

template <typename ArrayStruct>
std::vector<uint8_t> encode_fixed(MsgType type, const ArrayStruct& value) {
    return encode_envelope(
        type, std::vector<uint8_t>(value.bytes.begin(), value.bytes.end()));
}

template <typename ArrayStruct>
ArrayStruct decode_fixed(const std::vector<uint8_t>& payload) {
    ArrayStruct value{};
    if (payload.size() != value.bytes.size())
        throw SerializationError("merge message: bad fixed-size payload");
    std::copy(payload.begin(), payload.end(), value.bytes.begin());
    return value;
}

} // namespace (anonymous)

// ── Dialogue ──────────────────────────────────────────────────────────────────

MergeDialogue::MergeDialogue(blockchain::MergeSession& session,
                             ParticipantCache&         cache,
                             MergeConfig               config)
    : session_(session), cache_(cache), config_(std::move(config)) {}

void MergeDialogue::fail(const std::string& why) noexcept {
    state_ = State::FAILED;
    error_ = why;
}

void MergeDialogue::prepare_own_side() {
    own_tip_ = session_.prepare_tip(config_.user_id, config_.leaf_index);
    // Captured BEFORE create_pending, which replaces the stored snapshot with
    // the union: both sides must merge the partner's pre-merge snapshot.
    own_snapshot_ = session_.snapshot_for(config_.user_id, config_.leaf_index);
}

MergeDialogue::Messages MergeDialogue::start() noexcept {
    if (state_ != State::IDLE) {
        fail("start(): dialogue already started");
        return {};
    }
    try {
        prepare_own_side();
        state_ = State::WAIT_ACCEPT;
        return {encode_tip_snapshot(MsgType::OFFER, own_tip_, own_snapshot_)};
    } catch (const std::exception& e) {
        fail(std::string("start(): ") + e.what());
        return {};
    }
}

MergeDialogue::Messages MergeDialogue::on_message(const uint8_t* data,
                                                  size_t len) noexcept {
    if (state_ == State::DONE || state_ == State::FAILED) return {};
    try {
        const Envelope env = decode_envelope(data, len);
        switch (state_) {
            case State::IDLE:
                if (env.type == MsgType::OFFER)  return handle_offer(env.payload);
                break;
            case State::WAIT_ACCEPT:
                if (env.type == MsgType::ACCEPT) return handle_accept(env.payload);
                break;
            case State::WAIT_DRAFT:
                if (env.type == MsgType::DRAFT)  return handle_draft(env.payload);
                break;
            case State::WAIT_COSIG:
                if (env.type == MsgType::COSIG)  return handle_cosig(env.payload);
                break;
            default:
                break;
        }
        throw SerializationError("message type not valid in current state");
    } catch (const std::exception& e) {
        fail(e.what());
        return {};
    }
}

void MergeDialogue::accept_partner(const std::vector<uint8_t>& payload) {
    decode_tip_snapshot(payload, partner_tip_, partner_snapshot_);
    session_.verify_partner_tip(partner_tip_);
}

MergeDialogue::Messages MergeDialogue::handle_offer(const std::vector<uint8_t>& payload) {
    accept_partner(payload);
    prepare_own_side();
    pending_ = session_.create_pending(config_.user_id, config_.leaf_index,
                                       partner_tip_, partner_snapshot_,
                                       config_.working_keypair,
                                       config_.merge_timestamp,
                                       config_.validated_depth);
    state_ = State::WAIT_DRAFT;
    return {encode_tip_snapshot(MsgType::ACCEPT, own_tip_, own_snapshot_),
            encode_fixed(MsgType::DRAFT, pending_->draft_hash)};
}

MergeDialogue::Messages MergeDialogue::handle_accept(const std::vector<uint8_t>& payload) {
    accept_partner(payload);
    pending_ = session_.create_pending(config_.user_id, config_.leaf_index,
                                       partner_tip_, partner_snapshot_,
                                       config_.working_keypair,
                                       config_.merge_timestamp,
                                       config_.validated_depth);
    state_ = State::WAIT_DRAFT;
    return {encode_fixed(MsgType::DRAFT, pending_->draft_hash)};
}

MergeDialogue::Messages MergeDialogue::handle_draft(const std::vector<uint8_t>& payload) {
    const Hash partner_draft = decode_fixed<Hash>(payload);
    const Signature co_sig = session_.co_sign(partner_draft, config_.working_keypair);
    state_ = State::WAIT_COSIG;
    return {encode_fixed(MsgType::COSIG, co_sig)};
}

MergeDialogue::Messages MergeDialogue::handle_cosig(const std::vector<uint8_t>& payload) {
    const Signature partner_co_sig = decode_fixed<Signature>(payload);
    // path is non-empty — verify_partner_tip guaranteed it.
    const PublicKey partner_wpk = partner_tip_.path.back().working_pubkey;
    merge_block_ = session_.finalize(*pending_, partner_co_sig, partner_wpk);
    session_.import_partner_data(partner_tip_);
    state_ = State::DONE;
    fill_cache();
    return {};
}

void MergeDialogue::fill_cache() noexcept {
    // Best-effort (§5.2): a cache hiccup must not invalidate a finalized merge —
    // a missing entry only means build_proof returns nullopt later.
    try {
        // A side's tip yields its single-leaf record only while its snapshot is
        // still that leaf (first merge). Composite snapshots carry leaves we
        // cannot see here; those arrive via gossip (§7).
        const auto put_leaf_if_fresh = [this](const BranchTipInfo& tip,
                                              const MergeSnapshot& snapshot) {
            if (!tip.tip_block.has_value()) return;
            const ExternalRef ref{tip.tip_address, tip.tip_hash};
            if (MerkleTree::leaf_hash(ref) == snapshot.merkle_root)
                cache_.put_leaf(LeafRecord{ref, tip.path, *tip.tip_block});
        };
        put_leaf_if_fresh(own_tip_, own_snapshot_);
        put_leaf_if_fresh(partner_tip_, partner_snapshot_);
        cache_.put_composition(own_snapshot_.merkle_root,
                               partner_snapshot_.merkle_root);
    } catch (...) {
    }
}

} // namespace chainsync
