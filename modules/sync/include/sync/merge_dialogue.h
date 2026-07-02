#pragma once

#include "sync/participant_cache.h"

#include <blockchain/merge_session.h>
#include <blockchain/merge_snapshot.h>
#include <blockchain/types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace chainsync {

// Per-side parameters of one merge attempt. Each side uses its own clock and
// declares its own validation depth (blockchain.md §6.5.5).
struct MergeConfig {
    blockchain::UserId    user_id;
    blockchain::NodeIndex leaf_index;
    blockchain::KeyPair   working_keypair;
    blockchain::Timestamp merge_timestamp;
    uint32_t              validated_depth;
};

// One bilateral merge as a transport-agnostic state machine (sync.md §6).
//
// Wraps the blockchain::MergeSession protocol (§6.4: prepare_tip →
// verify_partner_tip → create_pending → co_sign → finalize) into opaque byte
// messages, replacing the manual hex relay in the CLI. Any carrier that can
// deliver byte blobs between the two parties in order can run it; delivery,
// activity timeouts and retries are the transport's concern (§10.2) — a
// stalled dialogue is simply abandoned, which the merge DAG tolerates
// (blockchain.md §11.4).
//
// Wire flow (messages are deterministic-CBOR envelopes, see merge_dialogue.cpp):
//
//   initiator                                 responder
//   start()      ── OFFER  {tip, snapshot} ──►  on_message
//                ◄─ ACCEPT {tip, snapshot} ──
//                ◄─ DRAFT  {draft_hash}    ──
//   on_message   ── DRAFT  {draft_hash}    ──►
//   on_message   ── COSIG  {co_signature}  ──►
//                ◄─ COSIG  {co_signature}  ──
//
// Both sides exchange pre-merge snapshots, so both commit the same union root
// (MergeSnapshot::merge is commutative). On success each side finalizes its
// own MERGE block, imports the partner's path/tip and feeds the participant
// cache (§5.2): both single-leaf records when derivable from the exchanged
// tips, plus the composition own_root × partner_root.
//
// Failure semantics mirror FraudProof: start()/on_message never throw — any
// malformed message, protocol violation or failed verification moves the
// dialogue to FAILED (see error()) and it stops responding. Note that a
// failure after create_pending leaves the persisted draft and grown snapshot
// behind, as MergeSession documents.
class MergeDialogue {
public:
    using Messages = std::vector<std::vector<uint8_t>>;

    enum class State : uint8_t {
        IDLE,         // nothing happened yet; responder stays here until OFFER
        WAIT_ACCEPT,  // initiator sent OFFER
        WAIT_DRAFT,   // tips exchanged, own draft sent
        WAIT_COSIG,   // partner draft co-signed, awaiting ours back
        DONE,
        FAILED,
    };

    // session and cache must outlive the dialogue.
    MergeDialogue(blockchain::MergeSession& session,
                  ParticipantCache&         cache,
                  MergeConfig               config);

    // Initiator entry point: returns the OFFER to deliver to the partner.
    // Only valid once, from IDLE. Empty result means the dialogue FAILED
    // (e.g. own branch is empty, §6.4).
    Messages start() noexcept;

    // Feed one received message; returns the messages to send back (possibly
    // none). Messages after DONE/FAILED are ignored.
    Messages on_message(const uint8_t* data, size_t len) noexcept;

    State state()  const noexcept { return state_; }
    bool  done()   const noexcept { return state_ == State::DONE; }
    bool  failed() const noexcept { return state_ == State::FAILED; }

    // Why the dialogue failed; empty unless failed().
    const std::string& error() const noexcept { return error_; }

    // Own finalized MERGE block (co_signature set), present once done().
    const std::optional<blockchain::Block>& merge_block() const noexcept {
        return merge_block_;
    }

private:
    Messages handle_offer (const std::vector<uint8_t>& payload);
    Messages handle_accept(const std::vector<uint8_t>& payload);
    Messages handle_draft (const std::vector<uint8_t>& payload);
    Messages handle_cosig (const std::vector<uint8_t>& payload);

    void prepare_own_side();          // own tip + pre-merge snapshot
    void accept_partner(const std::vector<uint8_t>& payload);
    void fill_cache() noexcept;       // best-effort, §5.2
    void fail(const std::string& why) noexcept;

    blockchain::MergeSession& session_;
    ParticipantCache&         cache_;
    MergeConfig               config_;

    State       state_ = State::IDLE;
    std::string error_;

    blockchain::BranchTipInfo own_tip_;
    blockchain::MergeSnapshot own_snapshot_;      // pre-merge (as sent to partner)
    blockchain::BranchTipInfo partner_tip_;
    blockchain::MergeSnapshot partner_snapshot_;  // pre-merge (as received)

    std::optional<blockchain::PendingMergeBlock> pending_;
    std::optional<blockchain::Block>             merge_block_;
};

} // namespace chainsync
