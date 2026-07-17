#include "sync/dialogue_channel.h"

#include "wire_cbor.h"

#include <httplib.h>

#include <random>

namespace chainsync {

using blockchain::SerializationError;
using blockchain::UserId;

// ── Envelope codec ────────────────────────────────────────────────────────────

namespace {

constexpr uint64_t ENVELOPE_VERSION = 1;

std::string to_hex(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0xF];
    }
    return s;
}

} // namespace

std::vector<uint8_t> encode_relay_envelope(const RelayEnvelope& env) {
    std::vector<uint8_t> out;
    wire::put_head(out, 4, 5);   // array(5)
    wire::put_head(out, 0, ENVELOPE_VERSION);
    wire::put_bstr(out, {env.session.begin(), env.session.end()});
    wire::put_head(out, 0, env.seq);
    wire::put_bstr(out, {env.from.bytes.begin(), env.from.bytes.end()});
    wire::put_bstr(out, env.blob);
    return out;
}

RelayEnvelope decode_relay_envelope(const uint8_t* data, size_t len) {
    wire::CborReader r{data, len};
    if (r.head(4) != 5) throw SerializationError("relay envelope: bad array");
    if (r.head(0) != ENVELOPE_VERSION)
        throw SerializationError("relay envelope: unsupported version");
    RelayEnvelope env{};
    const auto session = r.bstr();
    if (session.size() != env.session.size())
        throw SerializationError("relay envelope: bad session size");
    std::copy(session.begin(), session.end(), env.session.begin());
    env.seq = r.head(0);
    const auto from = r.bstr();
    if (from.size() != env.from.bytes.size())
        throw SerializationError("relay envelope: bad sender size");
    std::copy(from.begin(), from.end(), env.from.bytes.begin());
    env.blob = r.bstr();
    r.expect_end();
    return env;
}

SessionId make_session_id() {
    // std::random_device, not crypto::: only collision-freedom matters — the
    // relay is untrusted anyway and the blobs are self-protected (§4.1).
    std::random_device rd;
    SessionId id{};
    for (auto& b : id) b = static_cast<uint8_t>(rd());
    return id;
}

// ── DialoguePump ──────────────────────────────────────────────────────────────

DialoguePump::DialoguePump(MergeDialogue&    dialogue,
                           IDialogueChannel& channel,
                           UserId            self,
                           UserId            peer,
                           SessionId         session)
    : dialogue_(dialogue), channel_(channel),
      self_(self), peer_(peer), session_(session) {}

bool DialoguePump::ship(MergeDialogue::Messages msgs) {
    for (auto& m : msgs) {
        RelayEnvelope env;
        env.session = session_;
        env.seq     = out_seq_++;
        env.from    = self_;
        env.blob    = std::move(m);
        if (!channel_.send(peer_, env)) return false;
    }
    return true;
}

bool DialoguePump::begin() {
    auto msgs = dialogue_.start();
    if (dialogue_.failed()) return false;
    return ship(std::move(msgs));
}

bool DialoguePump::feed(const RelayEnvelope& env) {
    if (env.session != session_ || !(env.from == peer_)) return true;  // foreign
    if (env.seq < next_in_ || reorder_.count(env.seq)) return true;    // duplicate
    reorder_.emplace(env.seq, env.blob);

    while (!finished()) {
        auto it = reorder_.find(next_in_);
        if (it == reorder_.end()) break;
        auto replies = dialogue_.on_message(it->second.data(), it->second.size());
        reorder_.erase(it);
        ++next_in_;
        if (!ship(std::move(replies))) return false;
    }
    return true;
}

// ── HttpDialogueChannel ───────────────────────────────────────────────────────

struct HttpDialogueChannel::Impl {
    httplib::Client cli;
    std::string     my_hex;

    Impl(const std::string& host, std::string my)
        : cli(host), my_hex(std::move(my)) {
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
    }
};

HttpDialogueChannel::HttpDialogueChannel(const std::string& base_url, UserId self) {
    // base_url goes to httplib::Client as-is: plain host:port, http:// or https://
    impl_ = std::make_unique<Impl>(
        base_url, to_hex(self.bytes.data(), self.bytes.size()));
}

HttpDialogueChannel::~HttpDialogueChannel() = default;

bool HttpDialogueChannel::send(const UserId& to, const RelayEnvelope& env) {
    const auto bytes = encode_relay_envelope(env);
    const auto res   = impl_->cli.Post(
        "/dialogue/" + to_hex(to.bytes.data(), to.bytes.size()),
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
        "application/cbor");
    return res && res->status == 200;
}

std::vector<RelayEnvelope> HttpDialogueChannel::poll() {
    const auto res = impl_->cli.Get("/dialogue/" + impl_->my_hex + "/inbox");
    if (!res || res->status != 200) return {};

    std::vector<RelayEnvelope> out;
    try {
        const auto* data = reinterpret_cast<const uint8_t*>(res->body.data());
        wire::CborReader r{data, res->body.size()};
        const uint64_t n = r.head(4);
        for (uint64_t i = 0; i < n; ++i) {
            const auto entry = r.bstr();
            try {
                out.push_back(decode_relay_envelope(entry.data(), entry.size()));
            } catch (const SerializationError&) {
                // Malformed mailbox entry — skip; the pump tolerates gaps.
            }
        }
        r.expect_end();
    } catch (const SerializationError&) {
        return out;   // truncated response: keep what parsed
    }
    return out;
}

} // namespace chainsync
