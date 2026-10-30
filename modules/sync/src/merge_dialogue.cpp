#include "sync/merge_dialogue.h"

#include "wire_cbor.h"

#include <blockchain/errors.h>
#include <blockchain/serializer.h>

namespace chainsync {

using blockchain::Block;
using blockchain::BranchTipInfo;
using blockchain::MergeSession;
using blockchain::MergeSnapshot;
using blockchain::SerializationError;
using blockchain::Serializer;

// ── Wire format ───────────────────────────────────────────────────────────────
//
// Every message is a deterministic-CBOR (RFC 8949 §4.2.1) envelope:
//
//   [ uint version, uint type, bstr payload ]
//
//   OFFER / ACCEPT payload: [ bstr encode(BranchTipInfo), bstr encode(MergeSnapshot) ]
//   HALF payload:           encode(Block) — the initiator's half, so that the
//                           responder reads what it answers: the tip taken and the time
//   ANSWER payload:         encode(Block) — the responder's half, which takes the
//                           initiator's half itself (blockchain.md §6.4)
//
// Decoding is strict: exact array sizes, minimal-length heads, no trailing
// bytes. Anything else is a protocol failure.

namespace {

using wire::CborReader;
using wire::put_bstr;
using wire::put_head;

constexpr uint64_t WIRE_VERSION = 2;

enum class MsgType : uint8_t { OFFER = 1, ACCEPT = 2, HALF = 3, ANSWER = 4 };

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
    // Captured BEFORE write_half, which replaces the stored snapshot with
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
            case State::WAIT_HALF:
                if (env.type == MsgType::HALF)   return handle_half(env.payload);
                break;
            case State::WAIT_ANSWER:
                if (env.type == MsgType::ANSWER) return handle_answer(env.payload);
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
    session_.check_partner_time(partner_tip_, config_.merge_timestamp);   // no merging with the future
}

MergeDialogue::Messages MergeDialogue::handle_offer(const std::vector<uint8_t>& payload) {
    accept_partner(payload);
    prepare_own_side();
    // Said now, not after the initiator has written its half.
    session_.check_own_branch(config_.user_id, config_.leaf_index, config_.working_keypair);
    state_ = State::WAIT_HALF;
    return {encode_tip_snapshot(MsgType::ACCEPT, own_tip_, own_snapshot_)};
}

MergeDialogue::Messages MergeDialogue::handle_accept(const std::vector<uint8_t>& payload) {
    accept_partner(payload);
    merge_block_ = session_.write_half(own_tip_, partner_tip_, partner_snapshot_,
                                       config_.working_keypair,
                                       config_.merge_timestamp,
                                       config_.validated_depth);
    state_ = State::WAIT_ANSWER;
    return {encode_envelope(MsgType::HALF, Serializer::encode(*merge_block_))};
}

MergeDialogue::Messages MergeDialogue::handle_half(const std::vector<uint8_t>& payload) {
    // Answer only what was read (blockchain.md §6.4): the partner's half takes our
    // tip, at a time that is now by our clock, over the same union.
    const Block half = Serializer::decode_block(payload.data(), payload.size());
    session_.check_partner_half(half, own_tip_, partner_tip_, config_.merge_timestamp,
                                MergeSnapshot::merge(own_snapshot_, partner_snapshot_));
    merge_block_ = session_.write_half(own_tip_, MergeSession::tip_after(partner_tip_, half),
                                       partner_snapshot_, config_.working_keypair,
                                       config_.merge_timestamp, config_.validated_depth);
    finish();
    return {encode_envelope(MsgType::ANSWER, Serializer::encode(*merge_block_))};
}

MergeDialogue::Messages MergeDialogue::handle_answer(const std::vector<uint8_t>& payload) {
    const Block answer = Serializer::decode_block(payload.data(), payload.size());
    session_.check_partner_half(answer, MergeSession::tip_after(own_tip_, *merge_block_),
                                partner_tip_, config_.merge_timestamp,
                                MergeSnapshot::merge(own_snapshot_, partner_snapshot_));
    finish();
    return {};
}

void MergeDialogue::finish() {
    session_.import_partner_data(partner_tip_);
    state_ = State::DONE;
    fill_cache();
}

void MergeDialogue::fill_cache() noexcept {
    // Best-effort (sync.md §5.2): a cache hiccup must not invalidate a written merge —
    // a missing entry only means build_proof returns nullopt later.
    try {
        record_merge(cache_, own_tip_, own_snapshot_, partner_tip_, partner_snapshot_);
    } catch (...) {
    }
}

} // namespace chainsync
