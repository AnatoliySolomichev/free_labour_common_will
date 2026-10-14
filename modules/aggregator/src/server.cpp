#include "aggregator/server.h"
#include "aggregator/attestation_view.h"
#include "aggregator/deal_view.h"
#include "aggregator/discovery_view.h"
#include "aggregator/economy_view.h"
#include "aggregator/footprint_view.h"
#include "aggregator/recognition.h"
#include "aggregator/resource_view.h"
#include "aggregator/match_view.h"
#include "aggregator/profile_view.h"
#include "aggregator/rates_view.h"
#include "aggregator/axis_ledger.h"
#include "aggregator/rent_map.h"
#include "aggregator/cloud_view.h"
#include "blockchain/serializer.h"
#include "blockchain/errors.h"
#include <map>
#include <set>
#include <records/catalog.h>
#include <records/codec.h>
#include <httplib.h>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

// JSON array of hex-encoded keys (attesters of a footprint, records.md §12.11).
std::string hex_array(const std::vector<blockchain::PublicKey>& keys) {
    std::string out = "[";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i) out += ',';
        out += '"' + to_hex(keys[i].bytes) + '"';
    }
    out += ']';
    return out;
}

// The full path of an axis (records.md §4): chain/node/block/hash — the form
// `bc accept --axis` takes.
std::string block_path_str(const records::BlockPath& p) {
    return to_hex(p.chain) + "/" + std::to_string(p.node) + "/"
         + std::to_string(p.block) + "/" + to_hex(p.hash);
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

// Whole file as text; nullopt when it cannot be read.
std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

// ── Profile JSON (records.md §8.6) ────────────────────────────────────────────
// Facet → JSON key. Plurals are spelled out: "industry" + "s" is not a word.
struct FacetKey { ProfileFacet facet; const char* key; };
constexpr FacetKey FACET_KEYS[] = {
    { ProfileFacet::Skill,      "skills"      },
    { ProfileFacet::Need,       "needs"       },
    { ProfileFacet::Aspiration, "aspirations" },
    { ProfileFacet::Industry,   "industries"  },
    { ProfileFacet::Hobby,      "hobbies"     },
    { ProfileFacet::Obstacle,   "obstacles"   },
};

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

// A list of strings as a JSON array.
std::string json_strings(const std::vector<std::string>& v) {
    std::string out = "[";
    for (std::size_t i = 0; i < v.size(); ++i)
        out += std::string(i ? "," : "") + "\"" + json_escape(v[i]) + "\"";
    return out + "]";
}

// One lot as JSON (records.md §9.8 v2): how much of an output went where.
std::string lot_json(const aggregator::LotSummary& l, const records::ResourceQty& o) {
    return "{\"out\":" + std::to_string(l.out) + ",\"resource\":\"" + json_escape(o.resource)
         + "\",\"qty\":" + std::to_string(l.qty) + ",\"unit\":\"" + json_escape(o.unit)
         + "\",\"sold\":" + std::to_string(l.sold)
         + ",\"written_off\":" + std::to_string(l.written_off)
         + ",\"consumed\":" + std::to_string(l.consumed)
         + ",\"reserved\":" + std::to_string(l.reserved)
         + ",\"remaining\":" + std::to_string(l.remaining)
         + ",\"faults\":" + std::to_string(l.faults)
         + ",\"gaps\":" + (l.gaps ? "true" : "false")
         + ",\"free_units\":" + json_strings(l.free_units) + "}";
}

// Tags are emitted verbatim: an attribute the view does not model (geo, radius,
// working hours) still reaches the consumer. That is the point — the profile
// convention evolves in docs and catalogs, not in this code.
std::string fact_to_json(const UserId& chain, const ProfileFact& f) {
    std::string s = "{\"slug\":\""      + json_escape(f.slug)
                  + "\",\"text\":\""    + json_escape(f.text)
                  + "\",\"ref\":\""     + to_hex(chain.bytes)
                  + "/"                 + to_hex(f.block_hash.bytes)
                  + "\",\"node_index\":" + std::to_string(f.address.node_index)
                  + ",\"block_index\":"  + std::to_string(f.address.block_index)
                  + ",\"closed\":"       + (f.closed ? "true" : "false")
                  + ",\"tags\":[";
    for (std::size_t i = 0; i < f.tags.size(); ++i) {
        if (i) s += ',';
        s += '"' + json_escape(f.tags[i]) + '"';
    }
    s += "]}";
    return s;
}

std::string profile_to_json(const UserId& chain, const ChainProfile& profile) {
    std::string s = "{\"chain\":\"" + to_hex(chain.bytes) + "\"";
    for (const auto& fk : FACET_KEYS) {
        s += ",\"";
        s += fk.key;
        s += "\":[";
        const auto facts = profile.by_facet(fk.facet);
        for (std::size_t i = 0; i < facts.size(); ++i) {
            if (i) s += ',';
            s += fact_to_json(chain, *facts[i]);
        }
        s += ']';
    }
    s += '}';
    return s;
}

// Accepted work of a worker, as the reader sees it (records.md §14.4): distinct
// accepting chains, hours, and the axes that work named most — labelled from
// the axis definitions in the chains. `null` when nobody accepted anything.
std::string attested_axes_json(
        const std::vector<AttestedAxis>&                            axes,
        const std::map<std::array<uint8_t, 32>, records::AxisDef>& defs) {
    std::string s = "[";
    for (std::size_t i = 0; i < axes.size(); ++i) {
        const auto dit = defs.find(axes[i].axis.hash);
        if (i) s += ',';
        s += "{\"axis\":\"" + block_path_str(axes[i].axis)
           + "\",\"label\":\""
           + json_escape(dit == defs.end() ? std::string{} : dit->second.ru)
           + "\",\"chains\":" + std::to_string(axes[i].chains)
           + ",\"hours\":"     + std::to_string(axes[i].hours) + "}";
    }
    return s + "]";
}

std::string attested_json(const AttestationView&                                   view,
                          const UserId&                                            worker,
                          const std::map<std::array<uint8_t, 32>, records::AxisDef>& defs) {
    const auto total = view.total(worker);
    if (total.chains == 0) return "null";
    auto axes = view.of(worker);
    if (axes.size() > 3) axes.resize(3);
    return "{\"chains\":" + std::to_string(total.chains)
         + ",\"hours\":"  + std::to_string(total.hours)
         + ",\"axes\":"   + attested_axes_json(axes, defs) + "}";
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
                                   std::filesystem::path own_chain_dir,
                                   std::filesystem::path catalog_dir)
    : storage_(storage)
    , port_(port)
    , peers_(std::move(peers))
    , sync_interval_(sync_interval)
    , mailbox_ttl_(mailbox_ttl)
    , catalog_dir_(std::move(catalog_dir))
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

    // ── Согласия на запись (ИР-012) ──────────────────────────────────────────
    //
    // Кто согласен с этой записью и кто против. Общего объекта «решение» нет:
    // порог считает читатель по N подписанным записям, как признанность
    // сделки. Позднее согласие цепи отменяет прежнее.
    svr.Get("/consents/:hash", [&](const httplib::Request& req, httplib::Response& res) {
        auto th = hex_to_hash(req.path_params.at("hash"));
        if (!th) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid hash\"}", "application/json");
            return;
        }
        std::map<UserId, std::tuple<bool, std::string, int64_t>> latest;
        for (const Hash& bh : storage_.all_block_hashes()) {
            const auto block = storage_.get_block_by_hash(bh);
            if (!block || block->type != blockchain::BlockType::DATA) continue;
            records::Record rec;
            try {
                rec = records::Codec::decode(block->payload.data(), block->payload.size());
            } catch (const records::CodecError&) { continue; }
            const auto* c = std::get_if<records::Consent>(&rec);
            if (!c || !(Hash{c->target.hash} == *th)) continue;
            const UserId who = block->address.user_id;
            if (c->target.chain == who.bytes) continue;      // себя не спрашивают
            const auto prev = latest.find(who);
            if (prev == latest.end() ||
                std::get<2>(prev->second) <= block->timestamp_claimed)
                latest[who] = {c->agreed, c->note, block->timestamp_claimed};
        }
        std::string body = "[";
        bool first = true;
        for (const auto& [who, v] : latest) {
            if (!first) body += ',';
            first = false;
            body += "{\"chain\":\""   + to_hex(who.bytes)
                 + "\",\"agreed\":"   + (std::get<0>(v) ? "true" : "false")
                 + ",\"note\":\""     + json_escape(std::get<1>(v))
                 + "\",\"at\":"       + std::to_string(std::get<2>(v))
                 + "}";
        }
        body += ']';
        res.set_content(body, "application/json");
    });

    // ── Кому сколько ресурса вещи принадлежит (ИР-013) ───────────────────────
    //
    // Доля — купленная порция ресурса, а не дробь: витрина складывает продажи
    // машино-часов, покупки и расход купленного. Производная, перепроверяемая
    // по сырым записям, как всё здесь (records.md §13).
    svr.Get("/resource/:chain/:hash", [&](const httplib::Request& req,
                                          httplib::Response& res) {
        auto ch = hex_to_hash(req.path_params.at("chain"));
        auto th = hex_to_hash(req.path_params.at("hash"));
        if (!ch || !th) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid chain or hash\"}", "application/json");
            return;
        }
        UserId chain{};
        chain.bytes = ch->bytes;
        try {
            const auto split = ResourceView::build(
                storage_, chain, th->bytes, static_cast<int64_t>(std::time(nullptr)));
            if (!split.known) {
                res.status = 404;
                res.set_content("{\"error\":\"средства производства с таким хешем "
                                "в этой цепи нет\"}", "application/json");
                return;
            }
            std::string body = "{\"chain\":\""   + to_hex(split.keeper.bytes)
                 + "\",\"tool\":\""             + to_hex(th->bytes)
                 + "\",\"name\":\""             + json_escape(split.name)
                 + "\",\"cost\":"               + std::to_string(split.cost)
                 + ",\"life\":"                  + std::to_string(split.life)
                 + ",\"days\":"                  + std::to_string(split.days)
                 + ",\"collected\":"             + std::to_string(split.collected)
                 + ",\"residual\":"              + std::to_string(split.residual)
                 + ",\"sold_hours\":"            + std::to_string(split.sold_hours)
                 + ",\"keeper_used\":"           + std::to_string(split.keeper_used)
                 + ",\"pending_hours\":"         + std::to_string(split.pending_hours)
                 + ",\"free_hours\":"            + std::to_string(split.free_hours)
                 + ",\"over_used\":"             + (split.over_used ? "true" : "false")
                 + ",\"holders\":[";
            for (std::size_t i = 0; i < split.holders.size(); ++i) {
                const auto& h = split.holders[i];
                if (i) body += ',';
                body += "{\"chain\":\""  + to_hex(h.chain.bytes)
                     + "\",\"hours\":"   + std::to_string(h.hours)
                     + ",\"paid\":"       + std::to_string(h.paid)
                     + ",\"spent\":"      + std::to_string(h.spent)
                     + ",\"left\":"       + std::to_string(h.left)
                     + ",\"deal\":\""    + to_hex(h.deal) + "\"}";
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── Пломбы как записи (blockchain.md §7.4 v2) ────────────────────────────
    //
    // Кто заверил эту запись: пломба лежит блоком в ветке свидетеля, поэтому
    // «чья она» говорит адрес блока — ни склада, ни привязки ключа к цепи не
    // требуется. Витрина производная: пересчитывается сканом, перепроверяется
    // по сырым цепям.
    svr.Get("/attests/:hash", [&](const httplib::Request& req, httplib::Response& res) {
        auto th = hex_to_hash(req.path_params.at("hash"));
        if (!th) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid hash\"}", "application/json");
            return;
        }
        std::string body = "[";
        bool first = true;
        for (const Hash& bh : storage_.all_block_hashes()) {
            const auto block = storage_.get_block_by_hash(bh);
            if (!block || block->type != blockchain::BlockType::DATA) continue;
            records::Record rec;
            try {
                rec = records::Codec::decode(block->payload.data(), block->payload.size());
            } catch (const records::CodecError&) { continue; }
            const auto* a = std::get_if<records::Attest>(&rec);
            if (!a || !(Hash{a->target.hash} == *th)) continue;
            if (a->target.chain == block->address.user_id.bytes) continue;  // себя не заверяют
            if (!first) body += ',';
            first = false;
            body += "{\"chain\":\""    + to_hex(block->address.user_id.bytes)
                 + "\",\"mode\":\""   + json_escape(a->mode)
                 + "\",\"note\":\""   + json_escape(a->note)
                 + "\",\"at\":"        + std::to_string(block->timestamp_claimed)
                 + ",\"block\":\""     + to_hex(bh.bytes)
                 + "\"}";
        }
        body += ']';
        res.set_content(body, "application/json");
    });

    // ── Key-path warehouse (sync.md §7.5; blockchain.md §7.5) ────────────────
    //
    // Who stands behind a seal: the node path from a chain's root down to the
    // key that signed it. Bytes in, bytes out — the warehouse never checks a
    // path, the reader runs KeyPathCert::verify itself. A forged path is
    // rejected by the reader, so lying here gains nothing.

    // Registered before /keypaths/:key — httplib matches in registration order.
    svr.Get("/keypaths/manifest", [&](const httplib::Request&, httplib::Response& res) {
        const auto keys = storage_.all_keypath_keys();
        std::string body = "{\"hashes\":[";  // signing keys; same shape as other manifests
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (i) body += ',';
            body += '"';
            body += to_hex(keys[i].bytes);
            body += '"';
        }
        body += "]}";
        res.set_content(body, "application/json");
    });

    svr.Post("/keypaths/:key", [&](const httplib::Request& req, httplib::Response& res) {
        auto k = hex_to_hash(req.path_params.at("key"));
        if (!k) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid key\"}", "application/json");
            return;
        }
        if (req.body.empty() || req.body.size() > kKeyPathMaxBytes) {
            res.status = 400;
            res.set_content("{\"error\":\"bad size\"}", "application/json");
            return;
        }
        PublicKey key{};
        key.bytes = k->bytes;
        try {
            if (storage_.get_keypath_bytes(key).size() >= kKeyPathsPerKeyCap) {
                res.status = 429;
                res.set_content("{\"error\":\"key path list full\"}", "application/json");
                return;
            }
            const bool stored = storage_.put_keypath_bytes(
                key, {req.body.begin(), req.body.end()});
            res.set_content(stored ? "{\"status\":\"stored\"}"
                                   : "{\"status\":\"duplicate\"}",
                            "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    svr.Get("/keypaths/:key", [&](const httplib::Request& req, httplib::Response& res) {
        auto k = hex_to_hash(req.path_params.at("key"));
        if (!k) { res.status = 400; return; }
        PublicKey key{};
        key.bytes = k->bytes;
        std::string body;
        const auto paths = storage_.get_keypath_bytes(key);
        cbor_array_head(body, paths.size());
        for (const auto& p : paths)
            cbor_bstr(body, std::string(p.begin(), p.end()));
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

    // Counterparty independence for the averaging (ИР-021). Same parameters on
    // both paths below — the ?day= preview must recompute by exactly the
    // methodology the published aggregate used, or re-checking is meaningless.
    static const IndependenceParams kIndependence{};
    // Committed alongside the result, like WorkCloud::params: a witness
    // must recompute by the same methodology, not guess which one was used.
    static const std::string kIndepParams =
        "v3;ref=axis_prices;window_days=" + std::to_string(kIndependence.window_days)
      + ";max_cycle="     + std::to_string(kIndependence.max_cycle)
      + ";kappa="         + std::to_string(kIndependence.kappa)
      + ";self_calibrate=" + std::string(kIndependence.self_calibrate ? "1" : "0")
      + ";calib_min_deals=" + std::to_string(kIndependence.calib_min_deals)
      + ";min_axis_payers=" + std::to_string(kIndependence.min_axis_payers);

    // GET /economy/rates — today's axis prices (records.md §11.2 v5).
    // Computed lazily once per day and published as a signed DailyAggregate
    // block in the aggregator's own chain; the block also enters the block
    // warehouse, so it gossips to peers and clients can fetch + verify it.
    // An axis's label comes from its definition in the chains (ИР-022) and is
    // for people only; the axis itself is its full path.
    svr.Get("/economy/rates", [&](const httplib::Request& req, httplib::Response& res) {
        if (!own_chain_) {
            res.status = 501;
            res.set_content("{\"error\":\"aggregator has no own chain\"}",
                            "application/json");
            return;
        }
        try {
            const auto defs = build_axis_definitions(storage_);
            const auto axes_json = [&](const std::vector<records::AxisPrice>& axes,
                                       bool with_weight) {
                std::string s = "[";
                for (size_t i = 0; i < axes.size(); ++i) {
                    const auto& p   = axes[i];
                    const auto  dit = defs.find(p.axis.hash);
                    if (i) s += ',';
                    s += "{\"axis\":\"" + block_path_str(p.axis)
                       + "\",\"label\":\""
                       + json_escape(dit == defs.end() ? std::string{} : dit->second.ru)
                       + "\",\"price\":" + std::to_string(p.price)
                       + ",\"hours\":"    + std::to_string(p.hours)
                       + ",\"deals\":"    + std::to_string(p.deals);
                    if (with_weight)
                        s += ",\"weighted_hours\":" + std::to_string(p.weighted_hours);
                    s += "}";
                }
                return s + "]";
            };

            // ?date=TS — the aggregate published for that day, as it was signed:
            // the prices a product made that day is valued at (ИР-023, вопрос 2).
            // The last one of the date stands (records.md §14.5).
            if (req.has_param("date")) {
                const int64_t d    = std::stoll(req.get_param_value("date"));
                const int64_t date = d - d % 86'400;
                std::optional<records::DailyAggregate> found;
                for (const Block& b : own_chain_->branch()) {
                    if (b.type != BlockType::DATA) continue;
                    records::Record rec;
                    try { rec = records::Codec::decode(b.payload.data(), b.payload.size()); }
                    catch (const records::CodecError&) { continue; }
                    if (const auto* agg = std::get_if<records::DailyAggregate>(&rec);
                        agg && agg->date == date)
                        found = *agg;
                }
                if (!found) {
                    res.status = 404;
                    res.set_content("{\"error\":\"no aggregate published for that day\"}",
                                    "application/json");
                    return;
                }
                res.set_content("{\"date\":" + std::to_string(found->date)
                                + ",\"W\":" + std::to_string(found->W)
                                + ",\"axes\":" + axes_json(found->axes, true) + "}",
                                "application/json");
                return;
            }

            // ?day=TS — preview: recompute the day over that day's deals without
            // publishing (re-checkability: compare with the published aggregate).
            if (req.has_param("day")) {
                const int64_t d  = std::stoll(req.get_param_value("day"));
                const auto    pr = build_daily_prices(storage_, d - d % 86'400, {},
                                                      0.3, 0.1, &kIndependence);
                res.set_content("{\"preview_day\":" + std::to_string(d)
                                + ",\"W\":" + std::to_string(pr.W)
                                + ",\"indep\":\"" + json_escape(kIndepParams)
                                + "\",\"axes\":" + axes_json(pr.axes, true) + "}",
                                "application/json");
                return;
            }

            std::lock_guard<std::mutex> lock(rates_mutex_);
            const auto now = static_cast<int64_t>(std::time(nullptr));
            const int64_t day = now - now % 86'400;

            std::optional<records::DailyAggregate> today;
            Hash today_hash{};
            std::vector<records::AxisPrice> previous;
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
                        previous  = d->axes;
                        prev_date = d->date;
                    }
                } catch (const records::CodecError&) {}
            }

            if (!today) {
                records::DailyAggregate d{};
                d.date      = day;
                d.timestamp = now;
                // Today's prices derive from YESTERDAY's settled deals
                // (main_ideas.pdf: коэффициенты этого дня → следующего).
                const auto pr = build_daily_prices(storage_, day - 86'400, previous,
                                                   0.3, 0.1, &kIndependence);
                d.axes  = pr.axes;
                d.W     = pr.W;
                d.indep = kIndepParams;
                const Block block = own_chain_->append_data(
                    records::Codec::encode(records::Record{d}));
                try { storage_.add_block(block); } catch (...) {}
                today      = std::move(d);
                today_hash = Crypto::hash_block(block);
            }

            res.set_content("{\"date\":" + std::to_string(today->date)
                            + ",\"chain\":\"" + to_hex(own_chain_->uid().bytes)
                            + "\",\"block\":\"" + to_hex(today_hash.bytes)
                            + "\",\"W\":" + std::to_string(today->W)
                            + ",\"axes\":" + axes_json(today->axes, false) + "}",
                            "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // GET /economy/cloud — the work cloud (records.md §11.7, specialty-axes.md
    // §10): samples of work — acceptances others named with `like` — and, per
    // sample, its k nearest by what their families were paid FOR (ИР-022 шаг 11).
    // Published once/day as a signed WorkCloud in the aggregator's own chain and
    // re-checkable via the committed `snapshot`. ?coords=1 adds the 2D map.
    svr.Get("/economy/cloud", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const auto comps = build_work_compositions(storage_);

            // snapshot commits the input: the block set the cloud was read off
            // (sorted hashes → deterministic).
            std::string input;
            {
                auto hs = storage_.all_block_hashes();
                std::sort(hs.begin(), hs.end(),
                          [](const Hash& a, const Hash& b) { return a.bytes < b.bytes; });
                for (const Hash& h : hs)
                    input.append(reinterpret_cast<const char*>(h.bytes.data()), 32);
            }
            const Hash snap = Crypto::hash(
                reinterpret_cast<const uint8_t*>(input.data()), input.size());

            const auto    now = static_cast<int64_t>(std::time(nullptr));
            const int64_t day = now - now % 86'400;
            std::vector<std::array<uint8_t, 32>> sources;
            if (own_chain_) sources.push_back(own_chain_->uid().bytes);

            const auto cloud = build_work_cloud(comps, day, now, snap.bytes, sources, 5);

            // Publish once/day (dedupe by scanning the branch), like DailyAggregate.
            std::string block_hex;
            if (own_chain_) {
                std::lock_guard<std::mutex> lock(rates_mutex_);
                bool have_today = false;
                for (const Block& b : own_chain_->branch()) {
                    if (b.type != BlockType::DATA) continue;
                    try {
                        const auto rec = records::Codec::decode(b.payload.data(),
                                                                b.payload.size());
                        if (const auto* c = std::get_if<records::WorkCloud>(&rec))
                            if (c->date == day) {
                                have_today = true;
                                block_hex  = to_hex(Crypto::hash_block(b).bytes);
                            }
                    } catch (const records::CodecError&) {}
                }
                if (!have_today) {
                    const Block block = own_chain_->append_data(
                        records::Codec::encode(records::Record{cloud}));
                    try { storage_.add_block(block); } catch (...) {}
                    block_hex = to_hex(Crypto::hash_block(block).bytes);
                }
            }

            // ?coords=1 — spectral map coordinates on demand, not stored in the
            // record (kept compact, specialty-axes.md §10).
            std::map<SampleKey, std::array<double, 2>> coords;
            if (req.has_param("coords")) coords = compute_spectral_coords(comps);

            const auto ref_str = [](const records::Ref& r) {
                return to_hex(r.chain) + "/" + to_hex(r.hash);
            };
            std::string body = "{\"date\":" + std::to_string(cloud.date)
                + ",\"snapshot\":\"" + to_hex(cloud.snapshot)
                + "\",\"block\":\"" + block_hex
                + "\",\"params\":\"" + json_escape(cloud.params)
                + "\",\"points\":[";
            bool first = true;
            for (const auto& p : cloud.points) {
                if (!first) body += ',';
                first = false;
                body += "{\"sample\":\"" + ref_str(p.sample)
                     + "\",\"title\":\"" + json_escape(p.title)
                     + "\",\"deals\":" + std::to_string(p.deals);
                if (const auto it = coords.find(p.sample.hash); it != coords.end())
                    body += ",\"coord\":[" + std::to_string(it->second[0]) + ","
                         + std::to_string(it->second[1]) + "]";
                body += ",\"neighbors\":[";
                bool nf = true;
                for (const auto& n : p.neighbors) {
                    if (!nf) body += ',';
                    nf = false;
                    const auto nit = comps.find(n.sample.hash);
                    body += "{\"sample\":\"" + ref_str(n.sample)
                         + "\",\"title\":\""
                         + json_escape(nit == comps.end() ? std::string{} : nit->second.title)
                         + "\",\"w\":" + std::to_string(n.weight) + "}";
                }
                body += "]}";
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // GET /economy/axis-ledger — куда труд сети ушёл НА САМОМ ДЕЛЕ (ИР-022).
    //
    // Не оценка и не подгонка: сделки сами расписали свою цену по осям, суммы
    // сошлись с ценой, здесь они просто сложены. Цена оси нигде не назначается —
    // сеть может сказать лишь, какая доля всех оплаченных часов ушла на
    // опасность, и это факт.
    //
    // `described: false` — оси, которой никто не дал определения. Это не ошибка:
    // заводить оси волен каждый. Это видимость того, что осью пользуются без
    // аргумента, — и чем чаще такую ось переиспользуют, тем громче сигнал.
    svr.Get("/economy/axis-ledger", [&](const httplib::Request& req,
                                        httplib::Response& res) {
        try {
            // Определения осей берутся ИЗ ЦЕПЕЙ, а не из файла, который возит с
            // собой агрегатор: словарь труда решают люди (ИР-022).
            const auto defs = build_axis_definitions(storage_);
            const auto led = build_axis_ledger(storage_, &defs);
            std::string body = "{\"axes\":[";
            for (size_t i = 0; i < led.size(); ++i) {
                const auto& r = led[i];
                if (i) body += ',';
                body += "{\"axis\":\"" + to_hex(r.axis.chain) + "/"
                                        + std::to_string(r.axis.node) + "/"
                                        + std::to_string(r.axis.block) + "/"
                                        + to_hex(r.axis.hash)
                     + "\",\"label\":\"" + json_escape(r.label)
                     + "\",\"units\":"    + std::to_string(r.units)
                     + ",\"hours\":"      + std::to_string(r.hours)
                     + ",\"share\":"      + std::to_string(r.share)
                     + ",\"per_hour\":"   + std::to_string(r.per_hour)
                     + ",\"spread\":"     + std::to_string(r.spread)
                     + ",\"deals\":"      + std::to_string(r.deals)
                     + ",\"chains\":"     + std::to_string(r.chains)
                     + ",\"described\":"  + (r.described ? "true" : "false")
                     + ",\"broken_paths\":" + std::to_string(r.broken_paths)
                     + ",\"path_ok\":"    + (r.path_ok ? "true" : "false") + "}";
            }
            body += "]";
            // ?rent=1 — карта ренты по книге осей (records.md §11.11): что корзина
            // заплатила сверх сетевой цены названных осей, как долго и КЕМ. Модели
            // нет: опора прочитана из тех же сделок, что и сама книга.
            if (req.has_param("rent")) {
                const auto now  = static_cast<int64_t>(std::time(nullptr));
                const auto rent = build_rent_map(
                    build_rent_input(storage_, now - 365LL * 86'400, now + 1));
                body += ",\"rent\":[";
                for (size_t i = 0; i < rent.size(); ++i) {
                    const auto& r = rent[i];
                    if (i) body += ',';
                    body += "{\"worker\":\"" + to_hex(r.worker)
                         + "\",\"title\":\""   + json_escape(r.title)
                         + "\",\"fact\":"       + std::to_string(r.fact)
                         + ",\"expected\":"     + std::to_string(r.expected)
                         + ",\"resid\":"        + std::to_string(r.resid)
                         + ",\"hours\":"        + std::to_string(r.hours)
                         + ",\"payers\":"       + std::to_string(r.payers)
                         + ",\"periods_over\":" + std::to_string(r.periods_over)
                         + ",\"periods_seen\":" + std::to_string(r.periods_seen)
                         // null, а не 0: «слишком тонко, чтобы судить» — не «один плательщик»
                         + (r.judged ? ",\"rent_payers\":" + std::to_string(r.rent_payers)
                                     : std::string(",\"rent_payers\":null"))
                         + "}";
                }
                body += "]";
            }
            body += "}";
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

    // Economic footprint, layers 2–3 (ИР-010, records.md §11.6). Served one
    // chain per request on purpose: the pairwise "who owes whom" map is
    // computable from the raw chains, but is not handed over on a plate.
    svr.Get("/economy/footprint/:uid", [&](const httplib::Request& req,
                                           httplib::Response& res) {
        auto uid_hash = hex_to_hash(req.path_params.at("uid"));
        if (!uid_hash) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid uid\"}", "application/json");
            return;
        }
        try {
            const auto view = FootprintView::build(storage_);
            UserId uid{};
            uid.bytes = uid_hash->bytes;
            const auto fp = view.chain(uid);
            if (!fp) {
                res.status = 404;
                res.set_content("{\"error\":\"chain left no economic trace\"}",
                                "application/json");
                return;
            }
            std::string holders;
            bool first = true;
            for (const auto& h : fp->holders) {
                if (!first) holders += ",";
                first = false;
                holders += "{\"chain\":\"" + to_hex(h.chain.bytes)
                         + "\",\"units\":" + std::to_string(h.units) + "}";
            }
            // Отрицательный остаток не прячется (economy.md §4.4): при проверке
            // векселей его быть не должно, и если он есть — это видно.
            std::string overdrawn;
            for (const auto& h : fp->overdrawn) {
                if (!overdrawn.empty()) overdrawn += ",";
                overdrawn += "{\"chain\":\"" + to_hex(h.chain.bytes)
                           + "\",\"units\":" + std::to_string(h.units) + "}";
            }
            const std::string body =
                  "{\"holders\":["            + holders + "]"
                + ",\"paper_outstanding\":"   + std::to_string(fp->paper_outstanding)
                + ",\"redeemers\":"           + std::to_string(fp->redeemers)
                + ",\"redeemed_to_others\":"  + std::to_string(fp->redeemed_to_others)
                + ",\"acceptors\":"           + std::to_string(fp->acceptors)
                + ",\"labor_outward\":"       + std::to_string(fp->labor_outward)
                + ",\"labor_sealed\":"        + std::to_string(fp->labor_sealed)
                + ",\"attesters\":"           + std::to_string(fp->attesters)
                + ",\"attester_chains\":"      + hex_array(fp->attester_chains)
                + ",\"vouched_hours\":"        + std::to_string(fp->vouched_hours)
                + ",\"vouched_works\":"        + std::to_string(fp->vouched_works)
                + ",\"unbacked_deals\":"       + std::to_string(fp->unbacked_deals)
                + ",\"unbacked_units\":"       + std::to_string(fp->unbacked_units)
                + ",\"false_reversals\":"      + std::to_string(fp->false_reversals)
                + ",\"overdrawn\":["           + overdrawn + "]"
                + ",\"counterparties\":"      + std::to_string(fp->counterparties)
                + ",\"internal_turnover\":"   + std::to_string(fp->internal_turnover)
                + ",\"boundary_turnover\":"   + std::to_string(fp->boundary_turnover)
                + ",\"has_turnover\":"        + (fp->has_turnover() ? "true" : "false")
                + ",\"closedness\":"          + std::to_string(fp->closedness())
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

    // ── Profiles (records.md §8.6/records.md §8.7, records.md §13) ──────────────────────────────────
    //
    // Participants' self-description, decoded from ordinary tagged Concepts —
    // skills, needs, aspirations, industries. Feeds manual need↔skill matching
    // and any external AI: the project embeds no LLM by design (ИР-005).
    // Derived and re-checkable, like every aggregator view.

    svr.Get("/profiles", [&](const httplib::Request&, httplib::Response& res) {
        try {
            const auto  view = ProfileView::build(storage_);
            std::string body = "{\"chains\":[";
            bool        first = true;
            for (const auto& [uid, profile] : view.chains()) {
                if (!first) body += ',';
                first = false;
                body += profile_to_json(uid, profile);
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    svr.Get("/profiles/:uid", [&](const httplib::Request& req,
                                  httplib::Response& res) {
        const auto uid_hash = hex_to_hash(req.path_params.at("uid"));
        if (!uid_hash) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid uid\"}", "application/json");
            return;
        }
        try {
            const auto view = ProfileView::build(storage_);
            UserId uid{};
            uid.bytes = uid_hash->bytes;
            const auto profile = view.chain(uid);
            if (!profile) {
                res.status = 404;
                res.set_content("{\"error\":\"chain has no profile records\"}",
                                "application/json");
                return;
            }
            res.set_content(profile_to_json(uid, *profile), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── Catalogs (records.md §8.7; docs/catalogs.md) ──────────────────────────
    //
    // Served verbatim from disk on every request — editing a catalog file needs
    // no restart. The files are the source; the aggregator only hands them out,
    // so all participants pick slugs from one version.

    svr.Get("/catalog", [&](const httplib::Request&, httplib::Response& res) {
        if (catalog_dir_.empty()) {
            res.status = 501;
            res.set_content("{\"error\":\"catalog dir not configured\"}",
                            "application/json");
            return;
        }
        try {
            // Bundle: {"professions": <file>, "needs": <file>, ...}. The files
            // are already JSON, so they are embedded, never re-serialized.
            std::string body = "{";
            bool        first = true;
            std::vector<std::filesystem::path> files;
            for (const auto& entry : std::filesystem::directory_iterator(catalog_dir_))
                if (entry.is_regular_file() && entry.path().extension() == ".json")
                    files.push_back(entry.path());
            std::sort(files.begin(), files.end());   // deterministic order

            for (const auto& path : files) {
                const auto text = read_file(path);
                if (!text) continue;
                if (!first) body += ',';
                first = false;
                body += '"' + json_escape(path.stem().string()) + "\":" + *text;
            }
            body += '}';
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    svr.Get("/catalog/:name", [&](const httplib::Request& req,
                                  httplib::Response& res) {
        if (catalog_dir_.empty()) {
            res.status = 501;
            res.set_content("{\"error\":\"catalog dir not configured\"}",
                            "application/json");
            return;
        }
        const std::string name = req.path_params.at("name");
        // A name is a bare catalog name — never a path. Refuse anything that
        // could climb out of the directory.
        if (name.empty() || name.find('/') != std::string::npos ||
            name.find('\\') != std::string::npos || name.find("..") != std::string::npos) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid catalog name\"}", "application/json");
            return;
        }
        const auto text = read_file(catalog_dir_ / (name + ".json"));
        if (!text) {
            res.status = 404;
            res.set_content("{\"error\":\"no such catalog\"}", "application/json");
            return;
        }
        res.set_content(*text, "application/json");
    });

    // ── Directory: who offers a skill / who needs what (records.md §8.6) ──────
    //
    // The matching primitive, straight off ProfileView::by_slug. Closed facts
    // drop out. Advisory and re-checkable, like every derived view.

    svr.Get("/directory/:facet/:slug", [&](const httplib::Request& req,
                                           httplib::Response& res) {
        const std::string facet_s = req.path_params.at("facet");
        const std::string slug    = req.path_params.at("slug");

        std::optional<ProfileFacet> facet;
        for (const auto& fk : FACET_KEYS)
            if (facet_s == fk.key) facet = fk.facet;    // "skills", "needs", ...
        if (!facet) {
            res.status = 400;
            res.set_content("{\"error\":\"unknown facet\"}", "application/json");
            return;
        }
        try {
            const auto  view = ProfileView::build(storage_);
            std::string body = "{\"facet\":\"" + json_escape(facet_s)
                             + "\",\"slug\":\"" + json_escape(slug)
                             + "\",\"chains\":[";
            // Accepted work next to the claim (records.md §14.4): the reader sees
            // the claimed skill and what others accepted, axis by axis, and decides.
            std::optional<AttestationView> attest;
            std::map<std::array<uint8_t, 32>, records::AxisDef> defs;
            if (*facet == ProfileFacet::Skill) {
                attest = AttestationView::build(storage_);
                defs   = build_axis_definitions(storage_);
            }
            bool first = true;
            for (const auto& uid : view.by_slug(*facet, slug)) {
                const auto profile = view.chain(uid);
                if (!profile) continue;
                for (const auto* fact : profile->by_facet(*facet)) {
                    if (fact->slug != slug || fact->closed) continue;
                    if (!first) body += ',';
                    first = false;
                    std::string one = fact_to_json(uid, *fact);
                    if (attest)
                        one.insert(one.size() - 1,
                                   ",\"attested\":" + attested_json(*attest, uid, defs));
                    body += one;
                }
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── GET /match (ИР-005) ───────────────────────────────────────────────────
    //
    // The payoff: whose needs are closed by whose skills, what nobody can close,
    // and who could grow into the gap. Needs the catalog (closed_by) — without
    // it a program cannot know that wiring is fixed by an electrician.

    svr.Get("/match", [&](const httplib::Request&, httplib::Response& res) {
        if (catalog_dir_.empty()) {
            res.status = 501;
            res.set_content(
                "{\"error\":\"matching needs a catalog: start with --catalog PATH\"}",
                "application/json");
            return;
        }
        try {
            const auto text = read_file(catalog_dir_ / "needs.json");
            if (!text) {
                res.status = 501;
                res.set_content("{\"error\":\"needs.json not found in catalog dir\"}",
                                "application/json");
                return;
            }
            const auto needs_catalog = records::parse_catalog(*text);

            ClosedBy closed_by;
            for (const auto& e : needs_catalog.entries)
                closed_by[e.slug] = e.closed_by;

            const auto view = MatchView::build(storage_, closed_by);
            const auto defs = build_axis_definitions(storage_);

            std::string body = "{\"needs\":[";
            bool first = true;
            for (const auto& m : view.needs()) {
                if (!first) body += ',';
                first = false;
                body += "{\"seeker\":\""    + to_hex(m.seeker.bytes)
                     + "\",\"ref\":\""      + to_hex(m.seeker.bytes) + "/"
                                            + to_hex(m.block_hash.bytes)
                     + "\",\"slug\":\""     + json_escape(m.slug)
                     + "\",\"text\":\""     + json_escape(m.text)
                     + "\",\"urgency\":\""  + json_escape(m.urgency)
                     + "\",\"candidates\":[";
                bool c_first = true;
                for (const auto& c : m.candidates) {
                    if (!c_first) body += ',';
                    c_first = false;
                    body += "{\"chain\":\""   + to_hex(c.chain.bytes)
                         + "\",\"slug\":\""   + json_escape(c.slug)
                         + "\",\"text\":\""   + json_escape(c.text)
                         + "\",\"grade\":\""  + json_escape(c.grade)
                         + "\",\"distance_km\":" + std::to_string(c.distance_km)
                         + ",\"attested_chains\":" + std::to_string(c.attested_chains)
                         + ",\"attested_hours\":"  + std::to_string(c.attested_hours)
                         + ",\"attested_axes\":"   + attested_axes_json(c.attested_axes, defs)
                         + "}";
                }
                body += "]}";
            }
            body += "],\"deficits\":[";
            first = true;
            for (const auto& d : view.deficits()) {
                if (!first) body += ',';
                first = false;
                body += "{\"need\":\""  + json_escape(d.need_slug)
                     + "\",\"text\":\"" + json_escape(d.text)
                     + "\",\"professions\":[";
                for (std::size_t i = 0; i < d.professions.size(); ++i)
                    body += (i ? ",\"" : "\"") + json_escape(d.professions[i]) + '"';
                body += "],\"aspiring\":[";
                for (std::size_t i = 0; i < d.aspiring.size(); ++i)
                    body += (i ? ",\"" : "\"") + to_hex(d.aspiring[i].bytes) + '"';
                body += "],\"willing\":[";
                for (std::size_t i = 0; i < d.willing.size(); ++i)
                    body += (i ? ",\"" : "\"") + to_hex(d.willing[i].bytes) + '"';
                body += "]}";
            }
            body += "],\"rings\":[";
            first = true;
            for (const auto& ring : view.rings()) {
                if (!first) body += ',';
                first = false;
                body += '[';
                for (std::size_t i = 0; i < ring.chains.size(); ++i)
                    body += (i ? ",\"" : "\"") + to_hex(ring.chains[i].bytes) + '"';
                body += ']';
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── GET /deals (records.md §8.6 «словарь сделки»; ИР-006 шаг 2) ───────────
    //
    // Every deal the aggregator can assemble: a need with its tail of links,
    // staged. Full refs are included on purpose — the CLI verbs act on them, so
    // nobody copies 64-hex hashes by hand. Advisory and re-checkable.

    svr.Get("/deals", [&](const httplib::Request&, httplib::Response& res) {
        try {
            const auto view = DealView::build(storage_);
            std::string body = "{\"deals\":[";
            bool first = true;
            for (const auto& d : view.deals()) {
                if (!first) body += ',';
                first = false;
                body += "{\"seeker\":\"" + to_hex(d.seeker.bytes) + "\"";
                if (d.need_hash)
                    body += ",\"need_ref\":\"" + to_hex(d.seeker.bytes) + "/"
                          + to_hex(d.need_hash->bytes) + "\"";
                body += ",\"slug\":\""    + json_escape(d.slug)
                      + "\",\"text\":\""  + json_escape(d.text)
                      + "\",\"urgency\":\"" + json_escape(d.urgency)
                      + "\",\"stage\":\"" + DealView::stage_name(d.stage())
                      + "\",\"closed\":"  + (d.closed ? "true" : "false")
                      + ",\"appraised\":" + std::to_string(d.appraised())
                      + ",\"paid\":"      + std::to_string(d.paid())
                      + ",\"takers\":[";
                bool f2 = true;
                for (const auto& t : d.takers) {
                    if (!f2) body += ',';
                    f2 = false;
                    body += "{\"chain\":\""      + to_hex(t.chain.bytes)
                          + "\",\"skill_ref\":\"" + to_hex(t.chain.bytes) + "/"
                          + to_hex(t.skill_hash.bytes) + "\"}";
                }
                body += "],\"pledges\":[";
                f2 = true;
                for (const auto& p : d.pledges) {
                    if (!f2) body += ',';
                    f2 = false;
                    body += "{\"pledger\":\"" + to_hex(p.pledger.bytes)
                          + "\",\"ref\":\""   + to_hex(p.pledger.bytes) + "/"
                          + to_hex(p.pledge_hash.bytes)
                          + "\",\"units\":"   + std::to_string(p.units)
                          + ",\"settled\":"   + std::to_string(p.settled)
                          + ",\"revoked\":"   + (p.revoked ? "true" : "false");
                    if (p.executor)
                        body += ",\"executor\":\"" + to_hex(p.executor->bytes) + "\"";
                    body += "}";
                }
                body += "],\"works\":[";
                f2 = true;
                for (const auto& w : d.works) {
                    if (!f2) body += ',';
                    f2 = false;
                    body += "{\"worker\":\"" + to_hex(w.worker.bytes)
                          + "\",\"ref\":\""  + to_hex(w.worker.bytes) + "/"
                          + to_hex(w.work_hash.bytes)
                          + "\",\"action\":\"" + json_escape(w.action)
                          + "\",\"hours\":"  + std::to_string(w.hours)
                          + ",\"accepted\":" + (w.accepted ? "true" : "false")
                          + ",\"acceptances\":" + std::to_string(w.acceptances)
                          + ",\"unaddressed\":" + std::to_string(w.unaddressed)
                          + ",\"oversold\":" + (w.oversold ? "true" : "false");
                    if (w.customer)
                        body += ",\"customer\":\"" + to_hex(w.customer->bytes) + "\"";
                    if (w.accepted)
                        body += ",\"acceptance_ref\":\"" + to_hex(w.acceptor.bytes)
                              + "/" + to_hex(w.acceptance_hash.bytes)
                              + "\",\"labor_units\":" + std::to_string(w.labor_units)
                              + ",\"carried\":" + std::to_string(w.carried)
                              + ",\"paid\":" + std::to_string(w.paid);
                    body += "}";
                }
                body += "]}";
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── GET /work/<chain>/<hash> ─────────────────────────────────────────────
    // One result and who it went to (records.md §9.5, records.md §9.8): the
    // work's customer, the releases of it and every deal naming it, each with
    // its standing. `bc accept` and `bc release` read it before signing.
    svr.Get("/work/:chain/:hash", [&](const httplib::Request& req,
                                      httplib::Response& res) {
        const auto chain = hex_to_hash(req.path_params.at("chain"));
        const auto hash  = hex_to_hash(req.path_params.at("hash"));
        if (!chain || !hash) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid ref\"}", "application/json");
            return;
        }
        try {
            const auto  deals = read_deals(storage_);
            const auto& ch    = chain->bytes;
            const auto& wh    = hash->bytes;
            const auto  w     = deals.works.find(wh);
            const bool  known = w != deals.works.end() && w->second.worker.bytes == ch;
            std::string body = "{\"ref\":\"" + to_hex(ch) + "/" + to_hex(wh) + "\""
                             + ",\"known\":" + (known ? "true" : "false");
            if (known && w->second.record.customer)
                body += ",\"customer\":\"" + to_hex(*w->second.record.customer) + "\"";
            body += ",\"releases\":[";
            bool first = true;
            for (const auto& r : deals.releases) {
                // The maker's releases and the holders' — a resale keeps the
                // product's birth record, so it is listed here too (v4).
                if (r.release.work.hash != wh || r.release.work.chain != ch) continue;
                if (!first) body += ',';
                first = false;
                body += "{\"ref\":\"" + to_hex(r.seller.bytes) + "/" + to_hex(r.hash)
                      + "\",\"seller\":\"" + to_hex(r.seller.bytes)
                      + "\",\"to\":\"" + to_hex(r.release.to)
                      + "\",\"expires\":" + std::to_string(r.release.expires)
                      + ",\"timestamp\":" + std::to_string(r.release.timestamp)
                      + ",\"taken\":" + (deals.taken.count(r.hash) ? "true" : "false");
                if (r.release.holding)     // a resale of this purchase (records.md §9.8 v4)
                    body += ",\"holding\":\"" + to_hex(r.release.holding->chain) + "/"
                          + to_hex(r.release.holding->hash) + "\"";
                if (r.release.addressed)   // an addressed buyout (records.md §9.8 v5)
                    body += ",\"addressed\":{\"to\":\"" + to_hex(r.release.addressed->to)
                          + "\",\"until\":" + std::to_string(r.release.addressed->until)
                          + ",\"wear\":" + (r.release.addressed->wear ? "true" : "false") + "}";
                if (r.release.lot) {       // a portion (records.md §9.8 v2)
                    const auto lot   = deals.lots.find(
                        {r.release.holding ? r.release.holding->hash : wh, r.release.lot->out});
                    const bool valid = lot != deals.lots.end() && lot->second.valid.count(r.hash);
                    body += ",\"out\":" + std::to_string(r.release.lot->out)
                          + ",\"qty\":" + std::to_string(r.release.lot->qty)
                          + ",\"after\":" + std::to_string(r.release.lot->after)
                          + ",\"valid\":" + (valid ? "true" : "false");
                    if (!r.release.lot->units.empty())
                        body += ",\"units\":" + json_strings(r.release.lot->units);
                }
                body += "}";
            }
            body += "],\"deals\":[";
            first = true;
            const auto& rc = deals.recognition;
            const auto  now = static_cast<int64_t>(std::time(nullptr));
            for (const auto& d : deals.all) {
                if (d.accept().work.hash != wh || d.accept().work.chain != ch) continue;
                // v6 (economy.md §4.4): an offer the seller has not accepted yet,
                // refused, let lapse, or one paid with paper nobody had moves
                // nothing — and is shown as such.
                const char* state = deal_standing(deals, d, now);
                double paid = 0;
                for (const auto& o : d.transfer.origins) paid += o.units;
                if (!first) body += ',';
                first = false;
                body += "{\"ref\":\"" + to_hex(d.payer.bytes) + "/" + to_hex(d.hash)
                      + "\",\"payer\":\"" + to_hex(d.payer.bytes)
                      + "\",\"timestamp\":" + std::to_string(d.transfer.timestamp)
                      + ",\"until\":" + std::to_string(d.transfer.until.value_or(0))
                      + ",\"state\":\"" + state + "\""
                      + ",\"paid\":" + std::to_string(paid);
                // What it bought from whom: the source is the maker's work, or the
                // holder's purchase for a resale (v4); a portion is a result of its own.
                std::string source = to_hex(ch) + "/" + to_hex(wh);
                if (d.accept().release) {
                    bool portion = false;
                    for (const auto& r : deals.releases)
                        if (r.hash == d.accept().release->hash) {
                            portion = r.release.lot.has_value();
                            if (r.release.holding)
                                source = to_hex(r.release.holding->chain) + "/"
                                       + to_hex(r.release.holding->hash);
                        }
                    for (const auto& p : deals.all)   // a buyback names the holder's purchase (v5)
                        if (p.hash == d.accept().release->hash && p.accept().buyback) {
                            portion = true;
                            source  = to_hex(p.payer.bytes) + "/" + to_hex(p.hash);
                        }
                    body += ",\"release\":\"" + to_hex(d.accept().release->chain) + "/"
                          + to_hex(d.accept().release->hash) + "\",\"portion\":"
                          + (portion ? "true" : "false");
                }
                body += ",\"source\":\"" + source + "\"}";
            }
            // Pre-signed returns of purchases of this product (records.md §9.8 v5).
            body += "],\"buybacks\":[";
            first = true;
            for (const auto& p : deals.all) {
                const auto& pa = p.accept();
                if (!pa.buyback || pa.work.hash != wh || pa.work.chain != ch ||
                    !deals.moves_hours(p.hash))
                    continue;
                if (!first) body += ',';
                first = false;
                std::string repair;   // работа держателя над этой покупкой (ИР-015)
                for (const auto& [rh, rw] : deals.works)
                    if (rw.record.improves && rw.record.improves->hash == p.hash &&
                        rw.record.improves->chain == p.payer.bytes && rw.worker == p.payer)
                        repair = to_hex(p.payer.bytes) + "/" + to_hex(rh);
                body += "{\"ref\":\"" + to_hex(p.payer.bytes) + "/" + to_hex(p.hash)
                      + "\",\"holder\":\"" + to_hex(p.payer.bytes)
                      + "\",\"to\":\"" + to_hex(pa.buyback->to)
                      + "\",\"until\":" + std::to_string(pa.buyback->until)
                      + ",\"wear\":" + (pa.buyback->wear ? "true" : "false")
                      + ",\"taken\":" + (deals.bought_back.count(p.hash) ? "true" : "false")
                      + (repair.empty() ? std::string{} : ",\"repair\":\"" + repair + "\"")
                      + "}";
            }
            body += "],\"oversold\":";
            body += rc.oversold.count(wh) ? "true" : "false";
            // The labor thread of the work (records.md §9.4 v4): a deal counts
            // only on a work in its worker's thread, on hours of its own.
            if (known) {
                const auto t = deals.threads.find(w->second.worker);
                const char* state = "sound";
                std::vector<std::array<uint8_t, 32>> crosses;
                if (t == deals.threads.end() || t->second.unthreaded.count(wh)) {
                    state = "unthreaded";
                } else if (t->second.broken.count(wh)) {
                    state = "broken";
                } else if (t->second.ahead.count(wh)) {
                    state = "ahead";               // records.md §9.4 v5: труд из будущего
                } else if (t->second.overlapping.count(wh)) {
                    state = "crossing";
                    for (const auto& [a, b] : t->second.overlaps) {
                        if (a == wh) crosses.push_back(b);
                        if (b == wh) crosses.push_back(a);
                    }
                }
                body += ",\"carry_sound\":" +
                        std::string(deals.carry_sound(wh) ? "true" : "false");
                body += ",\"thread_state\":\"" + std::string(state) + "\",\"crosses\":[";
                for (std::size_t i = 0; i < crosses.size(); ++i)
                    body += std::string(i ? "," : "") + "\"" + to_hex(ch) + "/"
                          + to_hex(crosses[i]) + "\"";
                body += "]";
                // The work's lots (records.md §9.8 v2): how much of each went where.
                const auto lots = lot_summaries(deals, wh);
                body += ",\"lots\":[";
                for (std::size_t i = 0; i < lots.size(); ++i)
                    body += std::string(i ? "," : "")
                          + lot_json(lots[i], w->second.record.outputs[lots[i].out]);
                body += "]";
            }
            body += "}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── GET /deal/<chain>/<hash> ─────────────────────────────────────────────
    // One deal as an offer (economy.md §4.4, records.md §11.13): its term, the
    // seller's answer that counted, and every portion with its bill — what the
    // buyer reads to learn the outcome and the seller to check the paper.
    svr.Get("/deal/:chain/:hash", [&](const httplib::Request& req,
                                      httplib::Response& res) {
        const auto chain = hex_to_hash(req.path_params.at("chain"));
        const auto hash  = hex_to_hash(req.path_params.at("hash"));
        if (!chain || !hash) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid ref\"}", "application/json");
            return;
        }
        try {
            const auto deals = read_deals(storage_);
            const auto now   = static_cast<int64_t>(std::time(nullptr));
            const DealEntry* d = nullptr;
            for (const auto& e : deals.all)
                if (e.hash == hash->bytes && e.payer.bytes == chain->bytes) d = &e;
            if (!d) {
                res.status = 404;
                res.set_content("{\"ref\":\"" + to_hex(chain->bytes) + "/" + to_hex(hash->bytes)
                                    + "\",\"known\":false}",
                                "application/json");
                return;
            }
            const auto& t = d->transfer;
            double paid = 0;
            for (const auto& o : t.origins) paid += o.units;
            const auto answer = deals.answer(d->hash);
            std::string body =
                "{\"ref\":\"" + to_hex(d->payer.bytes) + "/" + to_hex(d->hash) + "\""
                + ",\"known\":true"
                + ",\"payer\":\"" + to_hex(t.from) + "\""
                + ",\"seller\":\"" + to_hex(t.to) + "\""
                + ",\"to_node\":" + std::to_string(t.to_node)
                + ",\"node\":" + std::to_string(d->node)
                + ",\"timestamp\":" + std::to_string(t.timestamp)
                + ",\"until\":" + std::to_string(t.until.value_or(0))
                + ",\"price\":" + std::to_string(paid)
                + ",\"status\":\""
                + records::deal_status_name(records::deal_status(t, answer, now)) + "\""
                + ",\"standing\":\"" + deal_standing(deals, *d, now) + "\""
                + ",\"addressed\":"
                + (deals.recognition.unaddressed.count(d->hash) ? "false" : "true")
                + ",\"pre_accepted\":" + (deals.pre_accepted.count(d->hash) ? "true" : "false")
                + ",\"backed\":" + (deals.unbacked.count(d->hash) ? "false" : "true");
            if (const auto w = deals.unbacked_why.find(d->hash); w != deals.unbacked_why.end())
                body += ",\"unbacked_why\":\"" + json_escape(w->second) + "\"";
            if (const auto* r = deals.counted_reply(d->hash))
                body += ",\"reply\":{\"ref\":\"" + to_hex(r->seen.author) + "/" + to_hex(r->hash)
                      + "\",\"accepted\":" + (r->seen.reply.accepted ? "true" : "false")
                      + ",\"note\":\"" + json_escape(r->seen.reply.note)
                      + "\",\"timestamp\":" + std::to_string(r->seen.reply.timestamp) + "}";
            body += ",\"portions\":[";
            for (std::size_t i = 0; i < t.origins.size(); ++i) {
                const auto& o = t.origins[i];
                body += std::string(i ? "," : "") + "{\"issuer\":\"" + to_hex(o.issuer)
                      + "\",\"units\":" + std::to_string(o.units);
                if (o.src) {
                    body += ",\"src\":\"" + to_hex(o.src->chain) + "/" + to_hex(o.src->hash) + "\"";
                    if (o.prev)
                        body += ",\"prev\":\"" + to_hex(o.prev->chain) + "/" + to_hex(o.prev->hash)
                              + "\"";
                    const auto b = deals.bills.find(
                        BillKey{d->payer.bytes, o.src->chain, o.src->hash, o.issuer});
                    if (b != deals.bills.end())
                        body += ",\"bill\":{\"credited\":" + std::to_string(b->second.state.credited)
                              + ",\"remaining\":" + std::to_string(b->second.state.remaining())
                              + ",\"backed\":"
                              + (b->second.state.backed.count(d->hash) ? "true" : "false")
                              + ",\"fault\":\"" + json_escape(b->second.state.fault) + "\"}";
                }
                body += "}";
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── GET /inbox/<chain> ───────────────────────────────────────────────────
    // Offers addressed to this chain that wait for its answer, each with its
    // term (economy.md §4.4). A void offer — not addressed — is nothing and is
    // not listed. A buyback paid with agreed paper needs no answer, but the holder
    // still takes it in — his own paper comes home and wants its receipt — so it
    // is listed, marked, until he has written anything about it.
    svr.Get("/inbox/:chain", [&](const httplib::Request& req, httplib::Response& res) {
        const auto chain = hex_to_hash(req.path_params.at("chain"));
        if (!chain) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid chain\"}", "application/json");
            return;
        }
        try {
            const auto deals = read_deals(storage_);
            const auto now   = static_cast<int64_t>(std::time(nullptr));
            std::string body = "{\"chain\":\"" + to_hex(chain->bytes) + "\",\"offers\":[";
            bool first = true;
            for (const auto& d : deals.all) {
                const auto& t = d.transfer;
                if (t.to != chain->bytes || deals.recognition.unaddressed.count(d.hash)) continue;
                const bool pre = deals.pre_accepted.count(d.hash) > 0;
                bool       said = false;   // the holder wrote anything about it
                if (const auto r = deals.replies.find(d.hash); r != deals.replies.end())
                    for (const auto& e : r->second) said = said || e.seen.author == t.to;
                if (pre ? said
                        : records::deal_status(t, deals.answer(d.hash), now) !=
                              records::DealStatus::pending)
                    continue;
                double paid = 0;
                for (const auto& o : t.origins) paid += o.units;
                body += std::string(first ? "" : ",") + "{\"ref\":\"" + to_hex(d.payer.bytes)
                      + "/" + to_hex(d.hash) + "\",\"payer\":\"" + to_hex(t.from)
                      + "\",\"price\":" + std::to_string(paid)
                      + ",\"timestamp\":" + std::to_string(t.timestamp)
                      + ",\"until\":" + std::to_string(t.until.value_or(0))
                      + ",\"to_node\":" + std::to_string(t.to_node)
                      + ",\"backed\":" + (deals.unbacked.count(d.hash) ? "false" : "true")
                      + ",\"pre_accepted\":" + (pre ? "true" : "false")
                      + ",\"work\":\"" + to_hex(d.accept().work.chain) + "/"
                      + to_hex(d.accept().work.hash) + "\"}";
                first = false;
            }
            body += "]}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── GET /thread/<chain> ──────────────────────────────────────────────────
    // A chain's goods thread, labor links (records.md §9.4 v4, ИР-023): its
    // works in time with their links, the faults — works outside the thread,
    // broken links, hours sold twice — and the labor no deal has confirmed yet,
    // by the axes the worker proposed. What a buyer reads before paying.
    svr.Get("/thread/:chain", [&](const httplib::Request& req,
                                  httplib::Response& res) {
        const auto chain = hex_to_hash(req.path_params.at("chain"));
        if (!chain) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid chain\"}", "application/json");
            return;
        }
        try {
            const auto deals = read_deals(storage_);
            UserId uid{};
            uid.bytes = chain->bytes;
            const auto t = deals.threads.find(uid);
            if (t == deals.threads.end()) {
                res.status = 404;
                res.set_content("{\"error\":\"chain wrote no work\"}", "application/json");
                return;
            }
            const records::LaborThread& th = t->second;
            const auto ref = [&](const std::array<uint8_t, 32>& h) {
                return to_hex(uid.bytes) + "/" + to_hex(h);
            };
            const auto label_of = [&](const records::BlockPath& p) -> std::string {
                const auto b = storage_.get_block_by_hash(Hash{p.hash});
                if (!b || b->type != BlockType::DATA) return {};
                try {
                    const auto rec = records::Codec::decode(b->payload.data(), b->payload.size());
                    if (const auto* d = std::get_if<records::AxisDef>(&rec))
                        return d->ru.empty() ? d->slug : d->ru;
                } catch (const records::CodecError&) {}
                return {};
            };

            std::vector<std::pair<std::array<uint8_t, 32>, const WorkOnTable*>> works;
            for (const auto& [h, w] : deals.works)
                if (w.worker == uid) works.emplace_back(h, &w);
            std::sort(works.begin(), works.end(), [](const auto& a, const auto& b) {
                return a.second->record.start_ts != b.second->record.start_ts
                     ? a.second->record.start_ts < b.second->record.start_ts
                     : a.first < b.first;
            });

            double unconfirmed = 0, without_axes = 0;
            std::map<std::string, std::pair<std::string, double>> by_axis;   // path → label, hours
            std::string body = "{\"chain\":\"" + to_hex(uid.bytes) + "\",\"works\":[";
            bool first = true;
            for (const auto& [h, w] : works) {
                const auto& r  = w->record;
                const auto  iv = records::labor_interval(r);
                const char* fault = th.broken.count(h)      ? "broken"
                                  : th.unthreaded.count(h)  ? "unthreaded"
                                  : th.ahead.count(h)       ? "ahead"
                                  : th.overlapping.count(h) ? "crossing" : "";
                if (!first) body += ',';
                first = false;
                body += "{\"ref\":\"" + ref(h) + "\",\"action\":\"" + json_escape(r.action)
                      + "\",\"start\":" + std::to_string(iv.first)
                      + ",\"end\":" + std::to_string(iv.second)
                      + ",\"hours\":" + std::to_string(r.hours)
                      + ",\"sold\":" + (deals.sold_whole.count(h) ? "true" : "false");
                if (r.thread)
                    body += ",\"seq\":" + std::to_string(r.thread->seq)
                          + ",\"prev\":" + std::to_string(r.thread->prev.size());
                if (r.customer) body += ",\"customer\":\"" + to_hex(*r.customer) + "\"";
                if (*fault) body += ",\"fault\":\"" + std::string(fault) + "\"";
                if (!r.customer && !r.outputs.empty()) {   // work for stock: its lots
                    const auto lots = lot_summaries(deals, h);
                    body += ",\"lots\":[";
                    for (std::size_t i = 0; i < lots.size(); ++i)
                        body += std::string(i ? "," : "")
                              + lot_json(lots[i], r.outputs[lots[i].out]);
                    body += "]";
                }
                body += ",\"axes\":[";
                for (std::size_t i = 0; i < r.axes.size(); ++i)
                    body += std::string(i ? "," : "") + "{\"axis\":\""
                          + block_path_str(r.axes[i].axis) + "\",\"share\":"
                          + std::to_string(r.axes[i].share) + "}";
                body += "]}";
                // Labor not yet confirmed: sound, and neither paid for whole nor sold
                // or written off in portions (records.md §9.8 v2).
                if (th.sound(h)) {
                    const double open = unconfirmed_hours(deals, h);
                    unconfirmed += open;
                    if (r.axes.empty()) without_axes += open;
                    for (const auto& x : r.axes) {
                        auto& slot = by_axis[block_path_str(x.axis)];
                        if (slot.first.empty()) slot.first = label_of(x.axis);
                        slot.second += x.share * open;
                    }
                }
            }
            body += "],\"tips\":[";
            for (std::size_t i = 0; i < th.tips.size(); ++i)
                body += std::string(i ? "," : "") + "\"" + ref(th.tips[i]) + "\"";
            body += "],\"forks\":" + std::to_string(th.forks)
                  + ",\"gaps\":" + (th.gaps ? "true" : "false") + ",\"overlaps\":[";
            for (std::size_t i = 0; i < th.overlaps.size(); ++i)
                body += std::string(i ? "," : "") + "[\"" + ref(th.overlaps[i].first)
                      + "\",\"" + ref(th.overlaps[i].second) + "\"]";
            std::vector<std::pair<std::string, std::pair<std::string, double>>> axes(
                by_axis.begin(), by_axis.end());
            std::sort(axes.begin(), axes.end(), [](const auto& a, const auto& b) {
                return a.second.second > b.second.second;
            });
            body += "],\"unconfirmed\":{\"hours\":" + std::to_string(unconfirmed)
                  + ",\"without_axes\":" + std::to_string(without_axes) + ",\"axes\":[";
            for (std::size_t i = 0; i < axes.size(); ++i)
                body += std::string(i ? "," : "") + "{\"axis\":\"" + axes[i].first
                      + "\",\"label\":\"" + json_escape(axes[i].second.first)
                      + "\",\"hours\":" + std::to_string(axes[i].second.second) + "}";
            body += "]}}";
            res.set_content(body, "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(std::string("{\"error\":\"") + e.what() + "\"}",
                            "application/json");
        }
    });

    // ── GET /stock/<chain> ───────────────────────────────────────────────────
    // What a chain bought and may sell on (records.md §9.8 v4): each purchase of
    // goods that happened, with its lots — sold on, written off, waiting for
    // payment, in stock — and what each cost: the price it is sold on at.
    svr.Get("/stock/:chain", [&](const httplib::Request& req, httplib::Response& res) {
        const auto chain = hex_to_hash(req.path_params.at("chain"));
        if (!chain) {
            res.status = 400;
            res.set_content("{\"error\":\"invalid chain\"}", "application/json");
            return;
        }
        try {
            const auto  deals = read_deals(storage_);
            std::string body  = "{\"chain\":\"" + to_hex(chain->bytes) + "\",\"holdings\":[";
            bool first = true;
            for (const auto& d : deals.all) {
                if (d.payer.bytes != chain->bytes) continue;
                const auto h = deals.holdings.find(d.hash);
                if (h == deals.holdings.end()) continue;
                const auto& work = d.accept().work;
                const auto  w    = deals.works.find(work.hash);
                if (w == deals.works.end()) continue;
                const auto lots = holding_summaries(deals, d.hash);
                if (!first) body += ',';
                first = false;
                body += "{\"ref\":\"" + to_hex(d.payer.bytes) + "/" + to_hex(d.hash)
                      + "\",\"work\":\"" + to_hex(work.chain) + "/" + to_hex(work.hash)
                      + "\",\"action\":\"" + json_escape(w->second.record.action) + "\"";
                if (const auto& b = d.accept().buyback)   // выкуп с адресом (records.md §9.8 v5)
                    body += ",\"buyback\":{\"to\":\"" + to_hex(b->to) + "\",\"until\":"
                          + std::to_string(b->until) + ",\"wear\":" + (b->wear ? "true" : "false")
                          + ",\"taken\":" + (deals.bought_back.count(d.hash) ? "true" : "false")
                          + "}";
                body += ",\"lots\":[";
                for (std::size_t i = 0; i < lots.size(); ++i) {
                    double cost = 0.0;
                    for (const auto& hl : h->second)
                        if (hl.out == lots[i].out) cost = hl.cost;
                    std::string lj = lot_json(lots[i], w->second.record.outputs[lots[i].out]);
                    lj.insert(lj.size() - 1, ",\"cost\":" + std::to_string(cost));
                    body += std::string(i ? "," : "") + lj;
                }
                body += "]}";
            }
            body += "]}";
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
    // peer_url examples: "http://192.168.1.2:8080", "https://agg.example.org".
    // httplib::Client is scheme-aware; the URL goes in as-is.
    httplib::Client cli(peer_url);
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

    // 5. Pull key paths (sync.md §7.5): who stands behind a seal. Same shape —
    // opaque bytes per signing key, verified by the reader's client, not here.
    for (const Hash& kh : manifest_hashes("/keypaths/manifest")) {
        auto res = cli.Get("/keypaths/" + to_hex(kh.bytes));
        if (!res || res->status != 200) continue;
        PublicKey key{};
        key.bytes = kh.bytes;
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
                if (storage_.put_keypath_bytes(
                        key, {body.begin() + pos, body.begin() + pos + len}))
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
