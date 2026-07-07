#include "sync/snapshot_exchange.h"
#include "user_ctx.h"

#include <aggregator/server.h>

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <thread>

using namespace blockchain;
using chainsync::Composition;
using chainsync::ISnapshotStore;
using chainsync::LeafRecord;
using chainsync::MergeDialogue;
using chainsync::ParticipantCache;
using chainsync::complete_cache;
using chainsync::publish_cache;
using sync_tests::LEAF;
using sync_tests::UserCtx;
using sync_tests::pump;

// ── In-memory warehouse ───────────────────────────────────────────────────────

struct HashLess {
    bool operator()(const Hash& a, const Hash& b) const noexcept {
        return a.bytes < b.bytes;
    }
};

struct InMemoryStore : ISnapshotStore {
    std::map<Hash, LeafRecord, HashLess>  leaves;
    std::map<Hash, Composition, HashLess> comps;

    bool put_leaf(const Hash& h, const LeafRecord& r) override {
        return leaves.emplace(h, r).second;
    }
    bool put_composition(const Hash& p, const Composition& c) override {
        return comps.emplace(p, c).second;
    }
    std::optional<LeafRecord> fetch_leaf(const Hash& h) override {
        auto it = leaves.find(h);
        if (it == leaves.end()) return std::nullopt;
        return it->second;
    }
    std::optional<Composition> fetch_composition(const Hash& p) override {
        auto it = comps.find(p);
        if (it == comps.end()) return std::nullopt;
        return it->second;
    }
};

// ── Fixture: the four-chain pairwise topology ─────────────────────────────────

class SnapshotExchangeTest : public ::testing::Test {
protected:
    std::filesystem::path    base_dir_;
    std::unique_ptr<UserCtx> alice_, bob_, carol_, dave_;
    Hash                     top_{};

    void SetUp() override {
        static int cnt = 0;
        base_dir_ = std::filesystem::temp_directory_path()
                  / ("sync_gossip_" + std::to_string(++cnt));
        std::filesystem::create_directories(base_dir_);
        alice_ = std::make_unique<UserCtx>(base_dir_, 1);
        bob_   = std::make_unique<UserCtx>(base_dir_, 2);
        carol_ = std::make_unique<UserCtx>(base_dir_, 3);
        dave_  = std::make_unique<UserCtx>(base_dir_, 4);
        alice_->append_block(0xAA, 1'000LL);
        bob_->append_block(0xBB, 1'000LL);
        carol_->append_block(0xCC, 1'000LL);
        dave_->append_block(0xDD, 1'000LL);
    }

    void TearDown() override {
        alice_.reset(); bob_.reset(); carol_.reset(); dave_.reset();
        std::filesystem::remove_all(base_dir_);
    }

    // A↔B, C↔D, then A↔C; returns leaving top root in top_.
    void merge_all() {
        MergeDialogue a1 = alice_->dialogue(2'000LL), b1 = bob_->dialogue(2'000LL);
        pump(a1, b1);
        ASSERT_TRUE(a1.done()) << a1.error();

        MergeDialogue c1 = carol_->dialogue(2'000LL), d1 = dave_->dialogue(2'000LL);
        pump(c1, d1);
        ASSERT_TRUE(c1.done()) << c1.error();

        MergeDialogue a2 = alice_->dialogue(3'000LL), c2 = carol_->dialogue(3'000LL);
        pump(a2, c2);
        ASSERT_TRUE(a2.done()) << a2.error();
        top_ = alice_->committed_root(*a2.merge_block());
    }

    Hash leaf_of(UserCtx& u) {
        Block b0 = u.bc->get_block({u.root_kp.pub, LEAF, 0});
        return MerkleTree::leaf_hash(u.leaf_ref_of(b0));
    }
};

// ── The gap gossip closes (§7.1) ──────────────────────────────────────────────

TEST_F(SnapshotExchangeTest, CompleteCacheMakesEveryParticipantProvable) {
    merge_all();

    // Everyone publishes what they saw; Alice's cache lacks Carol's half.
    InMemoryStore store;
    for (UserCtx* u : {alice_.get(), bob_.get(), carol_.get(), dave_.get()})
        publish_cache(store, u->cache);
    EXPECT_FALSE(alice_->cache.build_proof(top_, leaf_of(*carol_)).has_value());

    // Missing under the top root: the r_cd composition and both of its leaves
    // (round 2 exchanged composite snapshots, so no leaf crossed then).
    const std::size_t added = complete_cache(store, alice_->cache, top_);
    EXPECT_EQ(added, 3u);

    // Now every participant is provable against the committed top root.
    for (UserCtx* u : {alice_.get(), bob_.get(), carol_.get(), dave_.get()}) {
        const Hash leaf = leaf_of(*u);
        auto proof = alice_->cache.build_proof(top_, leaf);
        ASSERT_TRUE(proof.has_value());
        EXPECT_TRUE(MerkleTree::verify(leaf, proof->merkle_path, top_));
        EXPECT_EQ(FraudProof::verify_bad_sig(top_, *proof),
                  FraudVerdict::REFUTED_HONEST);
    }

    // Idempotent: nothing left to add.
    EXPECT_EQ(complete_cache(store, alice_->cache, top_), 0u);
}

TEST_F(SnapshotExchangeTest, PoisonedWarehouseEntriesAreDiscarded) {
    merge_all();

    InMemoryStore store;
    publish_cache(store, carol_->cache);

    // Poison: a composition that does not hash to its key, and a leaf under a
    // key it does not match. emplace-style maps keep first writes, so poison
    // must go in before honest data for the keys Alice will ask about.
    InMemoryStore poisoned;
    Composition bogus{};
    bogus.left_child.bytes.fill(0x11);
    bogus.right_child.bytes.fill(0x22);
    for (const auto& [root, comp] : store.comps) {
        (void)comp;
        poisoned.comps.emplace(root, bogus);
    }
    for (const auto& [hash, record] : store.leaves) {
        (void)hash;
        // Every leaf stored under some *other* leaf's key.
        LeafRecord wrong = store.leaves.begin()->second;
        poisoned.leaves.emplace(hash, wrong);
    }
    // Ensure at least one mismatched pair exists (two distinct leaves).
    ASSERT_GE(poisoned.leaves.size(), 2u);

    const std::size_t before = alice_->cache.leaf_count()
                             + alice_->cache.composition_count();
    // Poisoned entries fail key verification; at most the genuinely matching
    // first leaf (stored under its own key) slips through — and it is honest.
    complete_cache(poisoned, alice_->cache, top_);
    for (const auto& [hash, record] : alice_->cache.leaves()) {
        EXPECT_EQ(MerkleTree::leaf_hash(record.ref), hash);
    }
    for (const auto& [root, comp] : alice_->cache.compositions()) {
        EXPECT_EQ(MerkleTree::combine(comp.left_child, comp.right_child), root);
    }
    // Carol's half stayed unprovable — the poisoned warehouse gave nothing real.
    EXPECT_FALSE(alice_->cache.build_proof(top_, leaf_of(*dave_)).has_value());
    (void)before;
}

// ── Live HTTP warehouse ───────────────────────────────────────────────────────

TEST_F(SnapshotExchangeTest, FourChainsProvableThroughLiveHttpWarehouse) {
    merge_all();

    const uint16_t port = 18620;
    aggregator::AggregatorStorage relay_storage(base_dir_ / "warehouse_db");
    aggregator::AggregatorServer  server(relay_storage, port, {},
                                         std::chrono::seconds(3600));
    std::thread server_thread([&] { server.run(); });
    {
        httplib::Client probe("127.0.0.1:" + std::to_string(port));
        bool up = false;
        for (int i = 0; i < 100 && !up; ++i) {
            if (auto res = probe.Get("/stats"); res && res->status == 200) up = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(up) << "warehouse did not come up";
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port);
    chainsync::HttpSnapshotStore store(url);

    // Publishing is idempotent across users: shared entries stored once.
    std::size_t stored = 0;
    for (UserCtx* u : {alice_.get(), bob_.get(), carol_.get(), dave_.get()})
        stored += publish_cache(store, u->cache);
    // 4 distinct leaves + 3 distinct compositions in the whole DAG.
    EXPECT_EQ(stored, 7u);

    EXPECT_EQ(complete_cache(store, alice_->cache, top_), 3u);
    for (UserCtx* u : {alice_.get(), bob_.get(), carol_.get(), dave_.get()}) {
        const Hash leaf = leaf_of(*u);
        auto proof = alice_->cache.build_proof(top_, leaf);
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(FraudProof::verify_bad_sig(top_, *proof),
                  FraudVerdict::REFUTED_HONEST);
    }

    server.stop();
    server_thread.join();
}
