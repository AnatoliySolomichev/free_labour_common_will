#pragma once

#include "aggregator.h"
#include <chrono>
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
                     std::chrono::seconds     sync_interval);

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

    struct Impl; // holds httplib::Server (keeps httplib out of this header)
    std::unique_ptr<Impl> impl_;
};

} // namespace aggregator
