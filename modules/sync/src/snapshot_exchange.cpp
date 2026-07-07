#include "sync/snapshot_exchange.h"

#include <blockchain/errors.h>
#include <blockchain/merkle.h>

#include <httplib.h>

#include <cstring>
#include <unordered_set>

namespace chainsync {

using blockchain::Hash;
using blockchain::MerkleTree;
using blockchain::SerializationError;

// ── publish_cache ─────────────────────────────────────────────────────────────

std::size_t publish_cache(ISnapshotStore& store, const ParticipantCache& cache) {
    std::size_t stored = 0;
    for (const auto& [hash, record] : cache.leaves())
        if (store.put_leaf(hash, record)) ++stored;
    for (const auto& [root, comp] : cache.compositions())
        if (store.put_composition(root, comp)) ++stored;
    return stored;
}

// ── complete_cache ────────────────────────────────────────────────────────────

namespace {

struct HashKey {
    std::size_t operator()(const Hash& h) const noexcept {
        std::size_t v;
        std::memcpy(&v, h.bytes.data(), sizeof(v));
        return v;
    }
};

} // namespace

std::size_t complete_cache(ISnapshotStore& store, ParticipantCache& cache,
                           const Hash& root) {
    std::size_t added = 0;
    std::vector<Hash> stack{root};
    std::unordered_set<Hash, HashKey> visited;

    while (!stack.empty()) {
        const Hash node = stack.back();
        stack.pop_back();
        if (!visited.insert(node).second) continue;

        // Already-known composition: just descend.
        if (auto known = cache.get_composition(node)) {
            stack.push_back(known->left_child);
            stack.push_back(known->right_child);
            continue;
        }

        // Unknown node: a composition in the warehouse?
        if (auto comp = store.fetch_composition(node)) {
            // Verify against the key before trusting the children.
            if (MerkleTree::combine(comp->left_child, comp->right_child) == node) {
                cache.put_composition(comp->left_child, comp->right_child);
                ++added;
                stack.push_back(comp->left_child);
                stack.push_back(comp->right_child);
                continue;
            }
        }

        // Otherwise the node is (claimed to be) a leaf.
        if (!cache.get_leaf(node)) {
            if (auto record = store.fetch_leaf(node)) {
                try {
                    if (MerkleTree::leaf_hash(record->ref) == node) {
                        cache.put_leaf(*record);
                        ++added;
                    }
                } catch (const SerializationError&) {
                    // Unhashable garbage — drop.
                }
            }
        }
    }
    return added;
}

// ── HttpSnapshotStore ─────────────────────────────────────────────────────────

namespace {

std::string to_hex(const Hash& h) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(64);
    for (uint8_t b : h.bytes) {
        s += digits[b >> 4];
        s += digits[b & 0xF];
    }
    return s;
}

} // namespace

struct HttpSnapshotStore::Impl {
    httplib::Client cli;

    explicit Impl(const std::string& host) : cli(host) {
        cli.set_connection_timeout(5);
        cli.set_read_timeout(10);
    }
};

HttpSnapshotStore::HttpSnapshotStore(const std::string& base_url) {
    std::string host = base_url;
    if (host.rfind("http://", 0) == 0) host = host.substr(7);
    impl_ = std::make_unique<Impl>(host);
}

HttpSnapshotStore::~HttpSnapshotStore() = default;

bool HttpSnapshotStore::put_leaf(const Hash& leaf_hash, const LeafRecord& record) {
    std::vector<uint8_t> bytes;
    try {
        bytes = encode_leaf_record(record);
    } catch (const SerializationError&) {
        return false;
    }
    auto res = impl_->cli.Post(
        "/snapshot/leaf/" + to_hex(leaf_hash),
        std::string(bytes.begin(), bytes.end()), "application/octet-stream");
    // "duplicate" also answers 200 — only a fresh store counts.
    return res && res->status == 200
        && res->body.find("stored") != std::string::npos;
}

bool HttpSnapshotStore::put_composition(const Hash& parent_root,
                                        const Composition& composition) {
    std::string body;
    body.append(reinterpret_cast<const char*>(composition.left_child.bytes.data()), 32);
    body.append(reinterpret_cast<const char*>(composition.right_child.bytes.data()), 32);
    auto res = impl_->cli.Post("/snapshot/composition/" + to_hex(parent_root),
                               body, "application/octet-stream");
    return res && res->status == 200
        && res->body.find("stored") != std::string::npos;
}

std::optional<LeafRecord> HttpSnapshotStore::fetch_leaf(const Hash& leaf_hash) {
    auto res = impl_->cli.Get("/snapshot/leaf/" + to_hex(leaf_hash));
    if (!res || res->status != 200) return std::nullopt;
    try {
        return decode_leaf_record(
            reinterpret_cast<const uint8_t*>(res->body.data()), res->body.size());
    } catch (const SerializationError&) {
        return std::nullopt;   // warehouse garbage
    }
}

std::optional<Composition>
HttpSnapshotStore::fetch_composition(const Hash& parent_root) {
    auto res = impl_->cli.Get("/snapshot/composition/" + to_hex(parent_root));
    if (!res || res->status != 200 || res->body.size() != 64) return std::nullopt;
    Composition c{};
    std::memcpy(c.left_child.bytes.data(),  res->body.data(),      32);
    std::memcpy(c.right_child.bytes.data(), res->body.data() + 32, 32);
    return c;
}

} // namespace chainsync
