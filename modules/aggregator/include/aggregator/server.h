#pragma once

#include "aggregator.h"
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <atomic>
#include <thread>

namespace aggregator {

// ── AggregatorServer ──────────────────────────────────────────────────────────
//
// HTTP server exposing the aggregator API + background sync thread.
//
// Routes:
//   POST /blocks                  — submit a block (CBOR body)
//   GET  /ideas                   — all ideas (JSON)
//   GET  /ideas/{hash_hex}        — single idea (JSON)
//   GET  /blocks/{uid}/{ni}/{bi}  — single block (CBOR body)
//   GET  /sync/manifest           — list of known block hashes (JSON)
//   GET  /sync/block/{hash_hex}   — single block for peer sync (CBOR)
//   POST /dialogue/{uid}          — queue an opaque relay envelope (sync.md §4.1)
//   GET  /dialogue/{uid}/inbox    — drain the mailbox: CBOR array(bstr envelope)
//
// The dialogue mailbox is a dumb pipe: entries are stored and returned as raw
// bytes, never parsed or signed (sync.md §1). In-memory only — a lost envelope
// merely stalls a merge dialogue, which the DAG tolerates (blockchain.md
// §11.4). Entries expire after mailbox_ttl of sitting unread; a full mailbox
// (kMailboxCap) answers 429 — quota policy is open (sync.md §10.7).
//
// Sync:
//   Every sync_interval the server pulls new blocks from each peer by:
//   1. GET peer/sync/manifest → list of hashes
//   2. For each unknown hash: GET peer/sync/block/{hash} → Block → add_block()

class AggregatorServer {
public:
    AggregatorServer(AggregatorStorage& storage,
                     uint16_t           port,
                     std::vector<std::string> peer_urls,
                     std::chrono::seconds     sync_interval,
                     std::chrono::seconds     mailbox_ttl = std::chrono::seconds(3600));

    static constexpr std::size_t kMailboxCap       = 1024;  // envelopes per recipient
    static constexpr std::size_t kSealsPerBlockCap = 4096;  // seals per block

    ~AggregatorServer();

    // Blocking. Returns when stop() is called.
    void run();
    void stop();

private:
    void setup_routes();
    void start_sync_thread();
    void sync_loop();
    void sync_with_peer(const std::string& peer_url);

    AggregatorStorage&       storage_;
    uint16_t                 port_;
    std::vector<std::string> peers_;
    std::chrono::seconds     sync_interval_;
    std::atomic<bool>        running_{false};
    std::thread              sync_thread_;

    // Dialogue mailboxes: recipient uid (hex) → queued raw envelopes.
    struct MailboxEntry {
        std::string                           bytes;
        std::chrono::steady_clock::time_point queued_at;
    };
    std::mutex                                    mailbox_mutex_;
    std::map<std::string, std::vector<MailboxEntry>> mailboxes_;
    std::chrono::seconds                          mailbox_ttl_;

    struct Impl; // holds httplib::Server (keeps httplib out of this header)
    std::unique_ptr<Impl> impl_;
};

} // namespace aggregator
