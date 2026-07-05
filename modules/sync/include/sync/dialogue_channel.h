#pragma once

#include "sync/merge_dialogue.h"

#include <blockchain/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace chainsync {

// ── Relay envelope (sync.md §4.1) ─────────────────────────────────────────────
//
// Deterministic-CBOR wrapper the relay moves around without interpreting:
//
//   [ uint version, bstr session(16), uint seq, bstr from(32), bstr blob ]
//
// `session` is chosen by the initiator; `seq` counts per direction of a
// session, starting at 0; `from` tells the receiver where to send replies;
// `blob` is an opaque MergeDialogue message.

using SessionId = std::array<uint8_t, 16>;

struct RelayEnvelope {
    SessionId            session{};
    uint64_t             seq = 0;
    blockchain::UserId   from{};
    std::vector<uint8_t> blob;
};

std::vector<uint8_t> encode_relay_envelope(const RelayEnvelope& env);

// Strict decode; throws blockchain::SerializationError on malformed bytes.
RelayEnvelope decode_relay_envelope(const uint8_t* data, size_t len);

// Random session id (uniqueness, not secrecy — envelopes are self-protected).
SessionId make_session_id();

// ── Channel abstraction (§4.1) ────────────────────────────────────────────────
//
// A mailbox per user on some relay. poll() drains the own mailbox
// (at-most-once): a lost envelope only stalls the dialogue, which the merge
// DAG tolerates (blockchain.md §11.4). Both operations return transport
// failures as false/empty, never throw.

class IDialogueChannel {
public:
    virtual ~IDialogueChannel() = default;

    virtual bool send(const blockchain::UserId& to, const RelayEnvelope& env) = 0;

    virtual std::vector<RelayEnvelope> poll() = 0;
};

// ── DialoguePump ──────────────────────────────────────────────────────────────
//
// Drives one MergeDialogue over a channel: wraps outgoing messages into
// envelopes with a growing seq, and feeds incoming ones in seq order —
// duplicates are dropped, gaps are buffered, foreign session/sender envelopes
// are ignored. The dialogue itself stays byte-opaque to the transport.

class DialoguePump {
public:
    // dialogue and channel must outlive the pump.
    DialoguePump(MergeDialogue&     dialogue,
                 IDialogueChannel&  channel,
                 blockchain::UserId self,
                 blockchain::UserId peer,
                 SessionId          session);

    // Initiator entry point: ships the OFFER. False when the dialogue failed
    // to start or the transport rejected the send.
    bool begin();

    // Feed one polled envelope. False only on a transport send failure while
    // shipping replies (the dialogue is then stalled, not failed).
    bool feed(const RelayEnvelope& env);

    bool finished() const noexcept {
        return dialogue_.done() || dialogue_.failed();
    }

    const SessionId& session() const noexcept { return session_; }

private:
    bool ship(MergeDialogue::Messages msgs);

    MergeDialogue&     dialogue_;
    IDialogueChannel&  channel_;
    blockchain::UserId self_;
    blockchain::UserId peer_;
    SessionId          session_;

    uint64_t out_seq_ = 0;
    uint64_t next_in_ = 0;
    std::map<uint64_t, std::vector<uint8_t>> reorder_;   // buffered out-of-order
};

// ── HTTP channel over the aggregator mailbox relay (§4.1) ─────────────────────
//
//   POST /dialogue/{to_uid_hex}        body: CBOR envelope
//   GET  /dialogue/{my_uid_hex}/inbox  → CBOR array(bstr envelope), drains
//
// Malformed inbox entries are skipped. httplib stays out of this header.

class HttpDialogueChannel : public IDialogueChannel {
public:
    // base_url example: "http://127.0.0.1:8080"
    HttpDialogueChannel(const std::string& base_url, blockchain::UserId self);
    ~HttpDialogueChannel() override;

    bool send(const blockchain::UserId& to, const RelayEnvelope& env) override;
    std::vector<RelayEnvelope> poll() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace chainsync
