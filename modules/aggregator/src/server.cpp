#include "aggregator/server.h"
#include "aggregator/discovery_view.h"
#include "aggregator/economy_view.h"
#include "aggregator/rates_view.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <records/codec.h>
#include <httplib.h>
#include <algorithm>
#include <ctime>
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

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) out += ' ';
                else out += c;
        }
    }
    return out;
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
                                   std::chrono::seconds mailbox_ttl,
                                   std::filesystem::path own_chain_dir)
    : storage_(storage)
    , port_(port)
    , peers_(std::move(peers))
    , sync_interval_(sync_interval)
    , mailbox_ttl_(mailbox_ttl)
    , impl_(std::make_unique<Impl>())
{
    if (!own_chain_dir.empty()) {
        try {
            own_chain_ = std::make_unique<OwnChain>(own_chain_dir);
        } catch (const std::exception& e) {
            std::cerr << "[aggregator] own chain unavailable: " << e.what() << "\n";
        }
    }
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

    // ── Snapshot warehouse (sync.md §7.1) ─────────────────────────────────────
    //
    // Opaque leaf/composition bytes; the fetching node verifies entries
    // against their keys, the warehouse only stores. First write wins.

    auto snapshot_put = [&](const httplib::Request& req, httplib::Response& res,
                            bool is_leaf) {
        auto key = hex_to_hash(req.path_params.at("hash"));
        if (!key) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid key\"}", "application/json");
            return;
        }
        if (req.body.empty() || (!is_leaf && req.body.size() != 64)) {
            res.status = 400;
            res.set_content("{\"error\":\"bad size\"}", "application/json");
            return;
        }
        try {
            const std::vector<uint8_t> bytes(req.body.begin(), req.body.end());
            const bool stored = is_leaf
                ? storage_.put_snapshot_leaf(*key, bytes)
                : storage_.put_snapshot_composition(*key, bytes);
            res.set_content(stored ? "{\"status\":\"stored\"}"
                                   : "{\"status\":\"duplicate\"}",
                            "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    };

    auto snapshot_get = [&](const httplib::Request& req, httplib::Response& res,
                            bool is_leaf) {
        auto key = hex_to_hash(req.path_params.at("hash"));
        if (!key) { res.status = 400; return; }
        const auto bytes = is_leaf ? storage_.get_snapshot_leaf(*key)
                                   : storage_.get_snapshot_composition(*key);
        if (!bytes) { res.status = 404; return; }
        res.set_content(std::string(bytes->begin(), bytes->end()),
                        "application/octet-stream");
    };

    auto snapshot_manifest = [&](httplib::Response& res, bool is_leaf) {
        const auto hashes = is_leaf ? storage_.all_snapshot_leaf_hashes()
                                    : storage_.all_snapshot_composition_roots();
        std::string body = "{\"hashes\":[";
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            if (i) body += ',';
            body += '"';
            body += to_hex(hashes[i].bytes);
            body += '"';
        }
        body += "]}";
        res.set_content(body, "application/json");
    };

    svr.Post("/snapshot/leaf/:hash", [&, snapshot_put](const httplib::Request& req,
                                                       httplib::Response& res) {
        snapshot_put(req, res, true);
    });
    svr.Get("/snapshot/leaf/:hash", [&, snapshot_get](const httplib::Request& req,
                                                      httplib::Response& res) {
        snapshot_get(req, res, true);
    });
    svr.Post("/snapshot/composition/:hash", [&, snapshot_put](const httplib::Request& req,
                                                              httplib::Response& res) {
        snapshot_put(req, res, false);
    });
    svr.Get("/snapshot/composition/:hash", [&, snapshot_get](const httplib::Request& req,
                                                             httplib::Response& res) {
        snapshot_get(req, res, false);
    });
    svr.Get("/snapshot/leaves/manifest",
            [&, snapshot_manifest](const httplib::Request&, httplib::Response& res) {
        snapshot_manifest(res, true);
    });
    svr.Get("/snapshot/compositions/manifest",
            [&, snapshot_manifest](const httplib::Request&, httplib::Response& res) {
        snapshot_manifest(res, false);
    });

    // ── Seal warehouse (sync.md §7.2) ─────────────────────────────────────────
    //
    // Many seals per block; bytes in, bytes out — signatures are checked by
    // the fetching node, never here. Content-addressed dedupe in storage.

    // Registered before /seals/:hash — httplib matches in registration order.
    svr.Get("/seals/manifest", [&](const httplib::Request&, httplib::Response& res) {
        const auto hashes = storage_.all_sealed_block_hashes();
        std::string body = "{\"hashes\":[";
        for (std::size_t i = 0; i < hashes.size(); ++i) {
            if (i) body += ',';
            body += '"';
            body += to_hex(hashes[i].bytes);
            body += '"';
        }
        body += "]}";
        res.set_content(body, "application/json");
    });

    svr.Post("/seals/:hash", [&](const httplib::Request& req, httplib::Response& res) {
        auto bh = hex_to_hash(req.path_params.at("hash"));
        if (!bh) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid block hash\"}", "application/json");
            return;
        }
        if (req.body.empty() || req.body.size() > 4096) {
            res.status = 400;
            res.set_content("{\"error\":\"bad size\"}", "application/json");
            return;
        }
        try {
            if (storage_.get_seal_bytes(*bh).size() >= kSealsPerBlockCap) {
                res.status = 429;
                res.set_content("{\"error\":\"seal list full\"}", "application/json");
                return;
            }
            const bool stored = storage_.put_seal_bytes(
                *bh, {req.body.begin(), req.body.end()});
            res.set_content(stored ? "{\"status\":\"stored\"}"
                                   : "{\"status\":\"duplicate\"}",
                            "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    svr.Get("/seals/:hash", [&](const httplib::Request& req, httplib::Response& res) {
        auto bh = hex_to_hash(req.path_params.at("hash"));
        if (!bh) { res.status = 400; return; }
        std::string body;
        const auto seals = storage_.get_seal_bytes(*bh);
        cbor_array_head(body, seals.size());
        for (const auto& s : seals)
            cbor_bstr(body, std::string(s.begin(), s.end()));
        res.set_content(body, "application/cbor");
    });

    // ── Revocation warehouse (sync.md §7.2; blockchain.md §6.7 rule 8) ────────
    //
    // Self-verifying certificates; bytes in, bytes out — the fetching node runs
    // RevocationCert::verify itself, the warehouse never parses.

    // Registered before /revocations/:chain — httplib matches in registration order.
    svr.Get("/revocations/manifest", [&](const httplib::Request&, httplib::Response& res) {
        const auto chains = storage_.all_revoked_chains();
        std::string body = "{\"hashes\":[";  // chain ids; same shape as other manifests
        for (std::size_t i = 0; i < chains.size(); ++i) {
            if (i) body += ',';
            body += '"';
            body += to_hex(chains[i].bytes);
            body += '"';
        }
        body += "]}";
        res.set_content(body, "application/json");
    });

    svr.Post("/revocations/:chain", [&](const httplib::Request& req, httplib::Response& res) {
        auto ch = hex_to_hash(req.path_params.at("chain"));
        if (!ch) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid chain id\"}", "application/json");
            return;
        }
        if (req.body.empty() || req.body.size() > kRevocationCertMaxBytes) {
            res.status = 400;
            res.set_content("{\"error\":\"bad size\"}", "application/json");
            return;
        }
        UserId chain{};
        chain.bytes = ch->bytes;
        try {
            if (storage_.get_revocation_bytes(chain).size() >= kRevocationsPerChainCap) {
                res.status = 429;
                res.set_content("{\"error\":\"revocation list full\"}", "application/json");
                return;
            }
            const bool stored = storage_.put_revocation_bytes(
                chain, {req.body.begin(), req.body.end()});
            res.set_content(stored ? "{\"status\":\"stored\"}"
                                   : "{\"status\":\"duplicate\"}",
                            "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    svr.Get("/revocations/:chain", [&](const httplib::Request& req, httplib::Response& res) {
        auto ch = hex_to_hash(req.path_params.at("chain"));
        if (!ch) { res.status = 400; return; }
        UserId chain{};
        chain.bytes = ch->bytes;
        std::string body;
        const auto certs = storage_.get_revocation_bytes(chain);
        cbor_array_head(body, certs.size());
        for (const auto& c : certs)
            cbor_bstr(body, std::string(c.begin(), c.end()));
        res.set_content(body, "application/cbor");
    });

    // ── Economy view (records.md §13) ─────────────────────────────────────────
    //
    // Derived data, recomputed per request by scanning known blocks. Every
    // figure is re-checkable by any client against the signed chains.

    svr.Get("/economy/ideas", [&](const httplib::Request&, httplib::Response& res) {
        try {
            const auto view = EconomyView::build(
                storage_, static_cast<int64_t>(std::time(nullptr)));
            std::string body = "[";
            bool first = true;
            for (const auto& idea : view.ideas()) {
                if (!first) body += ',';
                first = false;
                body += "{\"idea\":\""        + to_hex(idea.idea_hash.bytes)
                     + "\",\"text\":\""       + json_escape(idea.text)
                     + "\",\"pledged_active\":"  + std::to_string(idea.pledged_active)
                     + ",\"pledged_settled\":"   + std::to_string(idea.pledged_settled)
                     + ",\"pledgers\":"          + std::to_string(idea.pledgers)
                     + ",\"copies\":"            + std::to_string(idea.copies)
                     + ",\"reactions\":"         + std::to_string(idea.reaction_sum)
                     + "}";
            }
            body += "]";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // GET /economy/rates — today's specialty rates (records.md §11.2).
    // Computed lazily once per day and published as a signed DailyAggregate
    // block in the aggregator's own chain; the block also enters the block
    // warehouse, so it gossips to peers and clients can fetch + verify it.
    svr.Get("/economy/rates", [&](const httplib::Request& req, httplib::Response& res) {
        if (!own_chain_) {
            res.status = 501;
            res.set_content("{\"error\":\"aggregator has no own chain\"}",
                            "application/json");
            return;
        }
        try {
            // ?day=TS — preview: recompute the raw day average over that
            // day's deals without publishing (re-checkability: compare with
            // the published aggregate).
            if (req.has_param("day")) {
                const int64_t d = std::stoll(req.get_param_value("day"));
                std::string body = "{\"preview_day\":" + std::to_string(d)
                                 + ",\"rates\":[";
                bool first = true;
                for (const auto& r : build_daily_rates(storage_, d - d % 86'400, {})) {
                    if (!first) body += ',';
                    first = false;
                    body += "{\"specialty\":\"" + json_escape(r.specialty)
                         + "\",\"level\":" + std::to_string(r.level)
                         + ",\"rate\":"    + std::to_string(r.rate)
                         + ",\"hours\":"   + std::to_string(r.hours)
                         + ",\"deals\":"   + std::to_string(r.deals)
                         + "}";
                }
                body += "]}";
                res.set_content(body, "application/json");
                return;
            }

            std::lock_guard<std::mutex> lock(rates_mutex_);
            const auto now = static_cast<int64_t>(std::time(nullptr));
            const int64_t day = now - now % 86'400;

            std::optional<records::DailyAggregate> today;
            Hash today_hash{};
            std::vector<records::RateEntry> previous;
            int64_t prev_date = -1;
            for (const Block& b : own_chain_->branch()) {
                if (b.type != BlockType::DATA) continue;
                try {
                    const auto rec = records::Codec::decode(b.payload.data(),
                                                            b.payload.size());
                    const auto* d = std::get_if<records::DailyAggregate>(&rec);
                    if (!d) continue;
                    if (d->date == day) {
                        today      = *d;   // a later block for today overrides
                        today_hash = Crypto::hash_block(b);
                    } else if (d->date < day && d->date >= prev_date) {
                        previous  = d->rates;
                        prev_date = d->date;
                    }
                } catch (const records::CodecError&) {}
            }

            if (!today) {
                records::DailyAggregate d{};
                d.date      = day;
                d.timestamp = now;
                // Today's rates derive from YESTERDAY's settled deals
                // (main_ideas.pdf: коэффициенты этого дня → следующего).
                d.rates     = build_daily_rates(storage_, day - 86'400, previous);
                const Block block = own_chain_->append_data(
                    records::Codec::encode(records::Record{d}));
                try { storage_.add_block(block); } catch (...) {}
                today      = std::move(d);
                today_hash = Crypto::hash_block(block);
            }

            std::string body = "{\"date\":" + std::to_string(today->date)
                + ",\"chain\":\"" + to_hex(own_chain_->uid().bytes)
                + "\",\"block\":\"" + to_hex(today_hash.bytes)
                + "\",\"rates\":[";
            bool first = true;
            for (const auto& r : today->rates) {
                if (!first) body += ',';
                first = false;
                body += "{\"specialty\":\"" + json_escape(r.specialty)
                     + "\",\"level\":" + std::to_string(r.level)
                     + ",\"rate\":"    + std::to_string(r.rate)
                     + ",\"hours\":"   + std::to_string(r.hours)
                     + ",\"deals\":"   + std::to_string(r.deals)
                     + "}";
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    svr.Get("/economy/chain/:uid", [&](const httplib::Request& req,
                                       httplib::Response& res) {
        auto uid_hash = hex_to_hash(req.path_params.at("uid"));
        if (!uid_hash) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid uid\"}", "application/json");
            return;
        }
        try {
            const auto view = EconomyView::build(
                storage_, static_cast<int64_t>(std::time(nullptr)));
            UserId uid{};
            uid.bytes = uid_hash->bytes;
            const auto chain = view.chain(uid);
            if (!chain) {
                res.status = 404;
                res.set_content("{\"error\":\"chain has no economic records\"}",
                                "application/json");
                return;
            }
            const std::string body =
                  "{\"debt\":"              + std::to_string(chain->debt())
                + ",\"issued\":"            + std::to_string(chain->issued)
                + ",\"redeemed\":"          + std::to_string(chain->redeemed)
                + ",\"received\":"          + std::to_string(chain->received)
                + ",\"spent\":"             + std::to_string(chain->spent)
                + ",\"pledges_active\":"    + std::to_string(chain->pledges_active)
                + ",\"pledges_settled\":"   + std::to_string(chain->pledges_settled)
                + ",\"pledges_revoked\":"   + std::to_string(chain->pledges_revoked)
                + ",\"pledges_expired\":"   + std::to_string(chain->pledges_expired)
                + ",\"works_accepted\":"    + std::to_string(chain->works_accepted)
                + ",\"labor_appraised\":"   + std::to_string(chain->labor_appraised)
                + "}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── Discovery (sync.md §8) ────────────────────────────────────────────────
    //
    // Ranked merge-partner suggestions for a chain. Advisory only: the merge
    // protocol protects itself, a biased index can merely skew suggestions.

    svr.Get("/discovery/:uid", [&](const httplib::Request& req,
                                   httplib::Response& res) {
        auto uid_hash = hex_to_hash(req.path_params.at("uid"));
        if (!uid_hash) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid uid\"}", "application/json");
            return;
        }
        try {
            const auto view = DiscoveryView::build(storage_);
            UserId uid{};
            uid.bytes = uid_hash->bytes;
            std::string body = "[";
            bool first = true;
            for (const auto& c : view.candidates_for(uid, 20)) {
                if (!first) body += ',';
                first = false;
                body += "{\"chain\":\""     + to_hex(c.chain.bytes)
                     + "\",\"score\":"      + std::to_string(c.score)
                     + ",\"econ_volume\":"  + std::to_string(c.econ_volume)
                     + ",\"neighbor\":"     + (c.neighbor ? "true" : "false")
                     + ",\"merges_with\":"  + std::to_string(c.merges_with)
                     + ",\"degree\":"       + std::to_string(c.degree)
                     + "}";
            }
            body += "]";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
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

    // Parse a JSON body for quoted 64-char hex strings (hand-written).
    auto manifest_hashes = [&cli](const std::string& path) {
        std::vector<Hash> hashes;
        auto res = cli.Get(path);
        if (!res || res->status != 200) return hashes;
        const std::string& body = res->body;
        std::size_t pos = 0;
        while ((pos = body.find('"', pos)) != std::string::npos) {
            std::size_t end = body.find('"', pos + 1);
            if (end == std::string::npos) break;
            std::string hex_str = body.substr(pos + 1, end - pos - 1);
            if (hex_str.size() == 64) {
                if (auto h = hex_to_hash(hex_str)) hashes.push_back(*h);
            }
            pos = end + 1;
        }
        return hashes;
    };

    // 1. Pull blocks we don't have (manifest + fetch).
    int pulled = 0;
    for (const Hash& bh : manifest_hashes("/sync/manifest")) {
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

    // 2. Pull snapshot leaves/compositions the same way (sync.md §7.1).
    // Bytes travel verbatim — the warehouse never interprets them.
    int gossiped = 0;
    for (const Hash& lh : manifest_hashes("/snapshot/leaves/manifest")) {
        if (storage_.get_snapshot_leaf(lh)) continue;
        auto res = cli.Get("/snapshot/leaf/" + to_hex(lh.bytes));
        if (!res || res->status != 200 || res->body.empty()) continue;
        try {
            if (storage_.put_snapshot_leaf(
                    lh, {res->body.begin(), res->body.end()})) ++gossiped;
        } catch (...) {}
    }
    for (const Hash& cr : manifest_hashes("/snapshot/compositions/manifest")) {
        if (storage_.get_snapshot_composition(cr)) continue;
        auto res = cli.Get("/snapshot/composition/" + to_hex(cr.bytes));
        if (!res || res->status != 200 || res->body.size() != 64) continue;
        try {
            if (storage_.put_snapshot_composition(
                    cr, {res->body.begin(), res->body.end()})) ++gossiped;
        } catch (...) {}
    }

    // 3. Pull seal lists the same way (sync.md §7.2): fetch each listed
    // block's seals and merge — content-addressed storage dedupes.
    for (const Hash& bh : manifest_hashes("/seals/manifest")) {
        auto res = cli.Get("/seals/" + to_hex(bh.bytes));
        if (!res || res->status != 200) continue;
        // CBOR array(bstr) — minimal parse mirroring the response encoder.
        const auto& body = res->body;
        std::size_t pos = 0;
        auto head = [&](uint8_t expect_major) -> long long {
            if (pos >= body.size()) return -1;
            const uint8_t b = static_cast<uint8_t>(body[pos++]);
            if ((b >> 5) != expect_major) return -1;
            const uint8_t ai = b & 0x1F;
            if (ai < 24) return ai;
            int extra = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : -1;
            if (extra < 0 || pos + extra > body.size()) return -1;
            long long v = 0;
            for (int i = 0; i < extra; ++i) v = (v << 8) | static_cast<uint8_t>(body[pos++]);
            return v;
        };
        const long long n = head(4);
        for (long long i = 0; i < n; ++i) {
            const long long len = head(2);
            if (len < 0 || pos + len > body.size()) break;
            try {
                if (storage_.put_seal_bytes(
                        bh, {body.begin() + pos, body.begin() + pos + len}))
                    ++gossiped;
            } catch (...) {}
            pos += static_cast<std::size_t>(len);
        }
    }

    // 4. Pull revocation certificates (sync.md §7.2): merge per-chain lists —
    // content-addressed storage dedupes; certificates are self-verifying and
    // checked by end clients, not here.
    for (const Hash& ch : manifest_hashes("/revocations/manifest")) {
        auto res = cli.Get("/revocations/" + to_hex(ch.bytes));
        if (!res || res->status != 200) continue;
        UserId chain{};
        chain.bytes = ch.bytes;
        const std::string& body = res->body;
        std::size_t pos = 0;
        auto head = [&](uint8_t expect_major) -> long long {
            if (pos >= body.size()) return -1;
            const uint8_t b = static_cast<uint8_t>(body[pos++]);
            if ((b >> 5) != expect_major) return -1;
            const uint8_t ai = b & 0x1F;
            if (ai < 24) return ai;
            int extra = ai == 24 ? 1 : ai == 25 ? 2 : ai == 26 ? 4 : ai == 27 ? 8 : -1;
            if (extra < 0 || pos + extra > body.size()) return -1;
            long long v = 0;
            for (int i = 0; i < extra; ++i) v = (v << 8) | static_cast<uint8_t>(body[pos++]);
            return v;
        };
        const long long n = head(4);
        for (long long i = 0; i < n; ++i) {
            const long long len = head(2);
            if (len < 0 || pos + len > body.size()) break;
            try {
                if (storage_.put_revocation_bytes(
                        chain, {body.begin() + pos, body.begin() + pos + len}))
                    ++gossiped;
            } catch (...) {}
            pos += static_cast<std::size_t>(len);
        }
    }

    if (pulled > 0 || gossiped > 0)
        std::cout << "[sync] pulled " << pulled << " block(s), " << gossiped
                  << " warehouse entr(ies) from " << peer_url << "\n";
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
