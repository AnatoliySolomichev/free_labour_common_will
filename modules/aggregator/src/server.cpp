#include "aggregator/server.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <httplib.h>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace aggregator {

// ── JSON helpers (hand-written — no extra dependency) ─────────────────────────

namespace {

std::string to_hex(const uint8_t* data, std::size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0xF];
    }
    return s;
}

template<std::size_t N>
std::string to_hex(const std::array<uint8_t,N>& a) {
    return to_hex(a.data(), N);
}

std::optional<Hash> hex_to_hash(const std::string& s) {
    if (s.size() != 64) return std::nullopt;
    Hash h{};
    for (int i = 0; i < 32; ++i) {
        auto nibble = [](char c) -> int {
            if (c>='0'&&c<='9') return c-'0';
            if (c>='a'&&c<='f') return c-'a'+10;
            if (c>='A'&&c<='F') return c-'A'+10;
            return -1;
        };
        int hi = nibble(s[2*i]), lo = nibble(s[2*i+1]);
        if (hi<0||lo<0) return std::nullopt;
        h.bytes[i] = static_cast<uint8_t>((hi<<4)|lo);
    }
    return h;
}

std::string address_to_json(const BlockAddress& a) {
    return "{\"user_id\":\"" + to_hex(a.user_id.bytes)
         + "\",\"node_index\":"  + std::to_string(a.node_index)
         + ",\"block_index\":"   + std::to_string(a.block_index)
         + "}";
}

std::string idea_to_json(const IdeaInfo& idea) {
    std::string s = "{\"idea_hash\":\"" + to_hex(idea.payload_hash.bytes)
                  + "\",\"witness_count\":"  + std::to_string(idea.witnesses.size())
                  + ",\"payload_hex\":\""    + to_hex(idea.payload.data(), idea.payload.size())
                  + "\",\"witnesses\":[";
    for (std::size_t i = 0; i < idea.witnesses.size(); ++i) {
        if (i) s += ',';
        s += address_to_json(idea.witnesses[i]);
    }
    s += "]}";
    return s;
}

// Deterministic-CBOR heads for the inbox response (array of byte strings).
void cbor_head(std::string& out, uint8_t major, uint64_t v) {
    const char m = static_cast<char>(major << 5);
    if (v < 24) { out.push_back(static_cast<char>(m | v)); return; }
    int extra = v <= 0xFF ? 1 : v <= 0xFFFF ? 2 : v <= 0xFFFF'FFFFull ? 4 : 8;
    out.push_back(static_cast<char>(m | (extra == 1 ? 24 : extra == 2 ? 25
                                       : extra == 4 ? 26 : 27)));
    for (int i = extra - 1; i >= 0; --i)
        out.push_back(static_cast<char>(v >> (8 * i)));
}

void cbor_array_head(std::string& out, uint64_t n) { cbor_head(out, 4, n); }

void cbor_bstr(std::string& out, const std::string& bytes) {
    cbor_head(out, 2, bytes.size());
    out += bytes;
}

} // anonymous namespace

// ── Impl (holds httplib::Server) ──────────────────────────────────────────────

struct AggregatorServer::Impl {
    httplib::Server svr;
};

// ── Constructor / Destructor ──────────────────────────────────────────────────

AggregatorServer::AggregatorServer(AggregatorStorage& storage,
                                   uint16_t port,
                                   std::vector<std::string> peers,
                                   std::chrono::seconds sync_interval,
                                   std::chrono::seconds mailbox_ttl)
    : storage_(storage)
    , port_(port)
    , peers_(std::move(peers))
    , sync_interval_(sync_interval)
    , mailbox_ttl_(mailbox_ttl)
    , impl_(std::make_unique<Impl>())
{
    setup_routes();
}

AggregatorServer::~AggregatorServer() {
    stop();
    if (sync_thread_.joinable()) sync_thread_.join();
}

// ── Routes ────────────────────────────────────────────────────────────────────

void AggregatorServer::setup_routes() {
    auto& svr = impl_->svr;

    // ── POST /blocks ──────────────────────────────────────────────────────────
    // Body: raw CBOR-encoded Block.
    svr.Post("/blocks", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto& body = req.body;
            Block b = Serializer::decode_block(
                reinterpret_cast<const uint8_t*>(body.data()), body.size());
            bool added = storage_.add_block(b);
            res.status = 200;
            res.set_content(
                added ? "{\"status\":\"accepted\"}" : "{\"status\":\"duplicate\"}",
                "application/json");
        } catch (const SerializationError& e) {
            res.status = 400;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
        }
    });

    // ── GET /ideas ────────────────────────────────────────────────────────────
    svr.Get("/ideas", [&](const httplib::Request&, httplib::Response& res) {
        try {
            auto ideas = storage_.all_ideas();
            std::string body = "[";
            for (std::size_t i = 0; i < ideas.size(); ++i) {
                if (i) body += ',';
                body += idea_to_json(ideas[i]);
            }
            body += "]";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}", "application/json");
        }
    });

    // ── GET /ideas/:hash ──────────────────────────────────────────────────────
    svr.Get("/ideas/:hash", [&](const httplib::Request& req, httplib::Response& res) {
        auto hash_opt = hex_to_hash(req.path_params.at("hash"));
        if (!hash_opt) { res.status = 400; res.set_content("{\"error\":\"invalid hash\"}", "application/json"); return; }
        auto idea = storage_.get_idea(*hash_opt);
        if (!idea)    { res.status = 404; res.set_content("{\"error\":\"not found\"}",    "application/json"); return; }
        res.set_content(idea_to_json(*idea), "application/json");
    });

    // ── GET /blocks/:uid/:ni/:bi ──────────────────────────────────────────────
    // Returns raw CBOR bytes of the block.
    svr.Get("/blocks/:uid/:ni/:bi", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            auto uid_opt = hex_to_hash(req.path_params.at("uid")); // Hash == PublicKey
            if (!uid_opt) { res.status = 400; return; }
            BlockAddress addr{};
            addr.user_id.bytes   = uid_opt->bytes;
            addr.node_index      = static_cast<NodeIndex>(std::stoul(req.path_params.at("ni")));
            addr.block_index     = static_cast<BlockIndex>(std::stoul(req.path_params.at("bi")));
            auto blk = storage_.get_block(addr);
            if (!blk) { res.status = 404; res.set_content("{\"error\":\"not found\"}", "application/json"); return; }
            auto cbor = Serializer::encode(*blk);
            res.set_content(std::string(reinterpret_cast<const char*>(cbor.data()), cbor.size()),
                            "application/octet-stream");
        } catch (...) { res.status = 400; }
    });

    // ── GET /sync/manifest ────────────────────────────────────────────────────
    svr.Get("/sync/manifest", [&](const httplib::Request&, httplib::Response& res) {
        auto hashes = storage_.all_block_hashes();
        std::string body = "{\"block_hashes\":[";
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            if (i) body += ',';
            body += '"';
            body += to_hex(hashes[i].bytes);
            body += '"';
        }
        body += "]}";
        res.set_content(body, "application/json");
    });

    // ── GET /sync/block/:hash ─────────────────────────────────────────────────
    svr.Get("/sync/block/:hash", [&](const httplib::Request& req, httplib::Response& res) {
        auto hash_opt = hex_to_hash(req.path_params.at("hash"));
        if (!hash_opt) { res.status = 400; return; }
        auto blk = storage_.get_block_by_hash(*hash_opt);
        if (!blk) { res.status = 404; return; }
        auto cbor = Serializer::encode(*blk);
        res.set_content(std::string(reinterpret_cast<const char*>(cbor.data()), cbor.size()),
                        "application/octet-stream");
    });

    // ── Dialogue mailbox relay (sync.md §4.1) ─────────────────────────────────
    //
    // A dumb pipe for merge-dialogue envelopes: bytes in, bytes out, nothing
    // parsed or signed. Expired entries are pruned lazily on access.

    auto prune_mailbox = [&](std::vector<MailboxEntry>& box) {
        const auto now = std::chrono::steady_clock::now();
        box.erase(std::remove_if(box.begin(), box.end(),
                                 [&](const MailboxEntry& e) {
                                     return now - e.queued_at >= mailbox_ttl_;
                                 }),
                  box.end());
    };

    // POST /dialogue/:uid — queue one envelope for :uid. Duplicate bodies are
    // accepted but stored once (idempotent retry).
    svr.Post("/dialogue/:uid", [&, prune_mailbox](const httplib::Request& req,
                                                  httplib::Response& res) {
        const std::string uid = req.path_params.at("uid");
        if (!hex_to_hash(uid)) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid uid\"}", "application/json");
            return;
        }
        std::lock_guard<std::mutex> lock(mailbox_mutex_);
        auto& box = mailboxes_[uid];
        prune_mailbox(box);
        for (const auto& e : box)
            if (e.bytes == req.body) {
                res.set_content("{\"status\":\"duplicate\"}", "application/json");
                return;
            }
        if (box.size() >= kMailboxCap) {
            res.status = 429;
            res.set_content("{\"error\":\"mailbox full\"}", "application/json");
            return;
        }
        box.push_back({req.body, std::chrono::steady_clock::now()});
        res.set_content("{\"status\":\"queued\"}", "application/json");
    });

    // GET /dialogue/:uid/inbox — drain the mailbox in arrival order.
    // Body: CBOR array of byte strings, one per envelope.
    svr.Get("/dialogue/:uid/inbox", [&, prune_mailbox](const httplib::Request& req,
                                                       httplib::Response& res) {
        const std::string uid = req.path_params.at("uid");
        if (!hex_to_hash(uid)) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid uid\"}", "application/json");
            return;
        }
        std::vector<MailboxEntry> box;
        {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            auto it = mailboxes_.find(uid);
            if (it != mailboxes_.end()) {
                prune_mailbox(it->second);
                box = std::move(it->second);
                mailboxes_.erase(it);
            }
        }
        std::string body;
        cbor_array_head(body, box.size());
        for (const auto& e : box) cbor_bstr(body, e.bytes);
        res.set_content(body, "application/cbor");
    });

    // ── GET /stats ────────────────────────────────────────────────────────────
    svr.Get("/stats", [&](const httplib::Request&, httplib::Response& res) {
        std::string body = "{\"blocks\":"  + std::to_string(storage_.block_count())
                         + ",\"ideas\":"  + std::to_string(storage_.idea_count())
                         + ",\"peers\":"  + std::to_string(peers_.size()) + "}";
        res.set_content(body, "application/json");
    });
}

// ── Sync ──────────────────────────────────────────────────────────────────────

void AggregatorServer::sync_with_peer(const std::string& peer_url) {
    // peer_url example: "http://192.168.1.2:8080"
    // Strip scheme to get host:port for httplib::Client.
    std::string host = peer_url;
    if (host.substr(0, 7) == "http://") host = host.substr(7);

    httplib::Client cli(host);
    cli.set_connection_timeout(5);
    cli.set_read_timeout(10);

    // 1. Get manifest.
    auto manifest_res = cli.Get("/sync/manifest");
    if (!manifest_res || manifest_res->status != 200) return;

    // Parse JSON array of hex strings (hand-written).
    const std::string& body = manifest_res->body;
    std::vector<Hash> remote_hashes;
    std::size_t pos = 0;
    while ((pos = body.find('"', pos)) != std::string::npos) {
        std::size_t end = body.find('"', pos + 1);
        if (end == std::string::npos) break;
        std::string hex_str = body.substr(pos + 1, end - pos - 1);
        if (hex_str.size() == 64) {
            if (auto h = hex_to_hash(hex_str)) remote_hashes.push_back(*h);
        }
        pos = end + 1;
    }

    // 2. Pull blocks we don't have.
    int pulled = 0;
    for (const Hash& bh : remote_hashes) {
        if (storage_.has_block_hash(bh)) continue;

        auto blk_res = cli.Get("/sync/block/" + to_hex(bh.bytes));
        if (!blk_res || blk_res->status != 200) continue;

        try {
            const auto& data = blk_res->body;
            Block b = Serializer::decode_block(
                reinterpret_cast<const uint8_t*>(data.data()), data.size());
            if (storage_.add_block(b)) ++pulled;
        } catch (...) {}
    }

    if (pulled > 0)
        std::cout << "[sync] pulled " << pulled << " block(s) from " << peer_url << "\n";
}

void AggregatorServer::sync_loop() {
    while (running_.load()) {
        for (const auto& peer : peers_) {
            if (!running_.load()) break;
            try { sync_with_peer(peer); }
            catch (...) {} // don't crash the loop on network errors
        }
        // Sleep in small increments so stop() is responsive.
        for (int i = 0; i < static_cast<int>(sync_interval_.count()) && running_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

// ── run / stop ────────────────────────────────────────────────────────────────

void AggregatorServer::run() {
    running_ = true;
    if (!peers_.empty())
        sync_thread_ = std::thread(&AggregatorServer::sync_loop, this);

    std::cout << "[aggregator] listening on port " << port_
              << "  blocks=" << storage_.block_count()
              << "  ideas="  << storage_.idea_count() << "\n";

    impl_->svr.listen("0.0.0.0", port_);
    running_ = false;
}

void AggregatorServer::stop() {
    running_ = false;
    impl_->svr.stop();
    if (sync_thread_.joinable()) sync_thread_.join();
}

} // namespace aggregator
