#include "sync/participant_cache.h"

#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/merge_snapshot.h>

#include <gtest/gtest.h>
#include <filesystem>
#include <map>

using namespace blockchain;
using chainsync::Composition;
using chainsync::LeafRecord;
using chainsync::ParticipantCache;

static constexpr NodeIndex LEAF = 0x7FFF'FFFFu;

static Hash make_hash(uint8_t fill) { Hash h; h.bytes.fill(fill); return h; }

// One participant chain: identity + node path to LEAF + a signed block-0.
struct Participant {
    KeyPair           root_kp;
    KeyPair           leaf_kp;
    Block             block0;   // validly signed block-0
    std::vector<Node> path;     // root..leaf nodes

    ExternalRef honest_ref() const {
        return ExternalRef{block0.address, Crypto::hash_block(block0)};
    }
    Block bad_block() const {
        Block b = block0;
        b.signature.bytes[0] ^= 0xFF;
        return b;
    }
    ExternalRef bad_ref() const {
        return ExternalRef{block0.address, Crypto::hash_block(bad_block())};
    }
};

// Several independent participant chains in one storage; a ParticipantCache
// is filled from them the way the merge orchestrator would (§5.2).
class ParticipantCacheTest : public ::testing::Test {
protected:
    std::filesystem::path        db_;
    std::unique_ptr<LmdbStorage> storage_;
    std::unique_ptr<Validator>   validator_;
    std::unique_ptr<Blockchain>  bc_;
    ParticipantCache             cache_;

    void SetUp() override {
        static int cnt = 0;
        db_ = std::filesystem::temp_directory_path()
            / ("sync_pcache_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_);
        storage_   = std::make_unique<LmdbStorage>(db_);
        validator_ = std::make_unique<Validator>(*storage_);
        bc_        = std::make_unique<Blockchain>(*storage_, *validator_);
    }

    void TearDown() override {
        bc_.reset(); validator_.reset(); storage_.reset();
        std::filesystem::remove_all(db_);
    }

    Participant make_participant(uint8_t seed) {
        Participant p;
        p.root_kp = Crypto::generate_keypair();
        bc_->create_identity(p.root_kp);
        std::map<NodeIndex, KeyPair> keys;
        keys[0] = p.root_kp;
        for (NodeIndex idx : path_indices(LEAF))
            if (keys.find(idx) == keys.end()) keys[idx] = Crypto::generate_keypair();
        bc_->ensure_path(p.root_kp.pub, LEAF, [&](NodeIndex i) { return keys.at(i); });
        p.leaf_kp = keys.at(LEAF);
        p.block0  = bc_->append_data_block(p.root_kp.pub, LEAF, {seed},
                                           p.leaf_kp, 1'000LL + seed);
        p.path    = bc_->get_path(p.root_kp.pub, LEAF);
        return p;
    }

    // Cache the leaf as committed by `ref` (which may commit a bad block or a
    // wrong hash), with `evidence` as the accompanying block.
    Hash cache_leaf(const Participant& p, const ExternalRef& ref, const Block& evidence) {
        return cache_.put_leaf(LeafRecord{ref, p.path, evidence});
    }
};

// ── Single-leaf snapshot (fresh participant, empty Merkle path) ───────────────

TEST_F(ParticipantCacheTest, SingleLeafBadSigConfirmed) {
    Participant p = make_participant(1);
    Block bad = p.bad_block();
    Hash leaf = cache_leaf(p, p.bad_ref(), bad);

    // For a fresh participant the snapshot root IS the leaf hash (§5.1).
    EXPECT_EQ(leaf, MergeSnapshot::leaf(p.bad_ref()).merkle_root);

    auto proof = cache_.build_proof(leaf, leaf);
    ASSERT_TRUE(proof.has_value());
    EXPECT_TRUE(proof->merkle_path.path.empty());
    EXPECT_EQ(FraudProof::verify_bad_sig(leaf, *proof), FraudVerdict::CONFIRMED);
}

TEST_F(ParticipantCacheTest, SingleLeafHonestRefuted) {
    Participant p = make_participant(2);
    Hash leaf = cache_leaf(p, p.honest_ref(), p.block0);

    auto proof = cache_.build_proof(leaf, leaf);
    ASSERT_TRUE(proof.has_value());
    EXPECT_EQ(FraudProof::verify_bad_sig(leaf, *proof), FraudVerdict::REFUTED_HONEST);
    EXPECT_EQ(FraudProof::verify_hash_mismatch(leaf, *proof), FraudVerdict::REFUTED_HONEST);
}

// ── Bilateral merge (one composition level) ───────────────────────────────────

TEST_F(ParticipantCacheTest, PairMergeMatchesMergeSnapshotAndProves) {
    Participant a = make_participant(3);   // honest
    Participant b = make_participant(4);   // committed a bad-signature block
    Hash la = cache_leaf(a, a.honest_ref(), a.block0);
    Hash lb = cache_leaf(b, b.bad_ref(), b.bad_block());

    Hash parent = cache_.put_composition(la, lb);

    // The cached composition reproduces the committed union root exactly.
    MergeSnapshot expected = MergeSnapshot::merge(MergeSnapshot::leaf(a.honest_ref()),
                                                  MergeSnapshot::leaf(b.bad_ref()));
    EXPECT_EQ(parent, expected.merkle_root);

    auto pb = cache_.build_proof(parent, lb);
    ASSERT_TRUE(pb.has_value());
    ASSERT_EQ(pb->merkle_path.path.size(), 1u);
    EXPECT_EQ(FraudProof::verify_bad_sig(parent, *pb), FraudVerdict::CONFIRMED);

    auto pa = cache_.build_proof(parent, la);
    ASSERT_TRUE(pa.has_value());
    EXPECT_EQ(FraudProof::verify_bad_sig(parent, *pa), FraudVerdict::REFUTED_HONEST);
}

TEST_F(ParticipantCacheTest, PairMergeHashMismatchViaSerializedBytes) {
    Participant a = make_participant(5);
    Participant b = make_participant(6);   // committed a wrong hash for its block
    ExternalRef lie{b.block0.address, make_hash(0xEE)};
    Hash la = cache_leaf(a, a.honest_ref(), a.block0);
    Hash lb = cache_leaf(b, lie, b.block0);   // evidence: the real block

    Hash parent = cache_.put_composition(la, lb);

    // End-to-end: bytes as they would travel inside a records::FraudClaim.
    auto bytes = cache_.build_proof_bytes(parent, lb);
    ASSERT_TRUE(bytes.has_value());
    EXPECT_EQ(FraudProof::verify("hash_mismatch", bytes->data(), bytes->size(), parent),
              FraudVerdict::CONFIRMED);
    EXPECT_EQ(FraudProof::verify("bad_sig", bytes->data(), bytes->size(), parent),
              FraudVerdict::REFUTED_FABRICATED);   // real block signs fine
}

// ── Hierarchical DAG (two composition levels) ─────────────────────────────────

TEST_F(ParticipantCacheTest, DeepDagProofForEveryLeaf) {
    Participant a = make_participant(7);
    Participant b = make_participant(8);
    Participant c = make_participant(9);   // the cheater
    Participant d = make_participant(10);
    Hash la = cache_leaf(a, a.honest_ref(), a.block0);
    Hash lb = cache_leaf(b, b.honest_ref(), b.block0);
    Hash lc = cache_leaf(c, c.bad_ref(), c.bad_block());
    Hash ld = cache_leaf(d, d.honest_ref(), d.block0);

    Hash r1  = cache_.put_composition(la, lb);
    Hash r2  = cache_.put_composition(lc, ld);
    Hash top = cache_.put_composition(r1, r2);

    for (Hash leaf : {la, lb, lc, ld}) {
        auto proof = cache_.build_proof(top, leaf);
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(proof->merkle_path.path.size(), 2u);
        EXPECT_EQ(FraudProof::verify_bad_sig(top, *proof),
                  leaf == lc ? FraudVerdict::CONFIRMED : FraudVerdict::REFUTED_HONEST);
    }

    // The same leaf is also provable against the intermediate root.
    auto mid = cache_.build_proof(r2, lc);
    ASSERT_TRUE(mid.has_value());
    EXPECT_EQ(mid->merkle_path.path.size(), 1u);
    EXPECT_EQ(FraudProof::verify_bad_sig(r2, *mid), FraudVerdict::CONFIRMED);
}

TEST_F(ParticipantCacheTest, DiamondDagStillProves) {
    // The same leaf reaches the top root through two different merge paths.
    Participant a = make_participant(11);
    Participant b = make_participant(12);
    Participant c = make_participant(13);
    Hash la = cache_leaf(a, a.honest_ref(), a.block0);
    Hash lb = cache_leaf(b, b.honest_ref(), b.block0);
    Hash lc = cache_leaf(c, c.honest_ref(), c.block0);

    Hash r1  = cache_.put_composition(la, lb);
    Hash r2  = cache_.put_composition(la, lc);
    Hash top = cache_.put_composition(r1, r2);

    auto path = cache_.merkle_path(top, la);
    ASSERT_TRUE(path.has_value());
    EXPECT_EQ(path->path.size(), 2u);
    EXPECT_TRUE(MerkleTree::verify(la, *path, top));
}

// ── Structure of the cache itself ─────────────────────────────────────────────

TEST_F(ParticipantCacheTest, CompositionIsCommutativeAndCanonical) {
    Hash a = make_hash(0xAA);
    Hash b = make_hash(0xBB);
    EXPECT_EQ(cache_.put_composition(a, b), cache_.put_composition(b, a));
    EXPECT_EQ(cache_.composition_count(), 1u);

    auto comp = cache_.get_composition(MerkleTree::combine(a, b));
    ASSERT_TRUE(comp.has_value());
    EXPECT_EQ(comp->left_child,  a);   // canonical order: smaller bytes = left
    EXPECT_EQ(comp->right_child, b);
}

TEST_F(ParticipantCacheTest, PutLeafIsIdempotentAndKeyed) {
    Participant p = make_participant(14);
    Hash k1 = cache_leaf(p, p.honest_ref(), p.block0);
    Hash k2 = cache_leaf(p, p.honest_ref(), p.block0);
    EXPECT_EQ(k1, k2);
    EXPECT_EQ(k1, MerkleTree::leaf_hash(p.honest_ref()));
    EXPECT_EQ(cache_.leaf_count(), 1u);

    auto rec = cache_.get_leaf(k1);
    ASSERT_TRUE(rec.has_value());
    EXPECT_EQ(rec->ref, p.honest_ref());
    EXPECT_EQ(rec->evidence.signature, p.block0.signature);
    EXPECT_EQ(rec->node_path.size(), p.path.size());
}

TEST_F(ParticipantCacheTest, MissingPiecesYieldNullopt) {
    Participant p = make_participant(15);
    Hash leaf = cache_leaf(p, p.honest_ref(), p.block0);

    // Unknown leaf → no proof, even with a valid structural path.
    EXPECT_FALSE(cache_.build_proof(make_hash(0x01), make_hash(0x01)).has_value());
    // Known leaf, but no composition chain to this root → not in that snapshot.
    EXPECT_FALSE(cache_.build_proof(make_hash(0x02), leaf).has_value());
    EXPECT_FALSE(cache_.merkle_path(make_hash(0x02), leaf).has_value());
    EXPECT_FALSE(cache_.get_leaf(make_hash(0x03)).has_value());
    EXPECT_FALSE(cache_.get_composition(make_hash(0x04)).has_value());

    // A root unrelated to the leaf, with compositions present, is still refused.
    cache_.put_composition(make_hash(0x05), make_hash(0x06));
    Hash other = MerkleTree::combine(make_hash(0x05), make_hash(0x06));
    EXPECT_FALSE(cache_.merkle_path(other, leaf).has_value());
}

TEST_F(ParticipantCacheTest, SelfTargetNeedsNoCompositions) {
    Hash h = make_hash(0x42);
    auto path = cache_.merkle_path(h, h);
    ASSERT_TRUE(path.has_value());
    EXPECT_TRUE(path->path.empty());
    EXPECT_TRUE(path->sibling_is_right.empty());
}

// ── Persistence (LMDB write-through) ──────────────────────────────────────────

TEST_F(ParticipantCacheTest, PersistentCacheSurvivesReopen) {
    Participant good = make_participant(16);
    Participant bad  = make_participant(17);   // committed a bad-signature block
    auto dir = db_.parent_path() / (db_.filename().string() + "_cache");
    std::filesystem::remove_all(dir);

    Hash lg, lb, parent;
    {
        ParticipantCache persistent(dir);
        lg = persistent.put_leaf(LeafRecord{good.honest_ref(), good.path, good.block0});
        lb = persistent.put_leaf(LeafRecord{bad.bad_ref(), bad.path, bad.bad_block()});
        parent = persistent.put_composition(lg, lb);
    }   // closed

    ParticipantCache reopened(dir);
    EXPECT_EQ(reopened.leaf_count(), 2u);
    EXPECT_EQ(reopened.composition_count(), 1u);

    // Full round-trip: the reloaded record still yields a verifiable proof.
    auto proof = reopened.build_proof(parent, lb);
    ASSERT_TRUE(proof.has_value());
    EXPECT_EQ(FraudProof::verify_bad_sig(parent, *proof), FraudVerdict::CONFIRMED);
    auto honest = reopened.build_proof(parent, lg);
    ASSERT_TRUE(honest.has_value());
    EXPECT_EQ(FraudProof::verify_bad_sig(parent, *honest), FraudVerdict::REFUTED_HONEST);

    std::filesystem::remove_all(dir);
}

// ── record_merge (§5.2 fill rule) ─────────────────────────────────────────────

TEST_F(ParticipantCacheTest, RecordMergeCachesFreshLeavesAndComposition) {
    Participant a = make_participant(18);
    Participant b = make_participant(19);

    const auto tip_of = [](const Participant& p) {
        return BranchTipInfo{p.block0.address, Crypto::hash_block(p.block0),
                             p.path, p.block0};
    };
    MergeSnapshot snap_a = MergeSnapshot::leaf(a.honest_ref());
    MergeSnapshot snap_b = MergeSnapshot::leaf(b.honest_ref());

    Hash root = chainsync::record_merge(cache_, tip_of(a), snap_a, tip_of(b), snap_b);
    EXPECT_EQ(root, MergeSnapshot::merge(snap_a, snap_b).merkle_root);
    EXPECT_EQ(cache_.leaf_count(), 2u);
    EXPECT_EQ(cache_.composition_count(), 1u);

    // A composite partner snapshot reveals no leaf: only the composition lands.
    Participant c = make_participant(20);
    MergeSnapshot composite = MergeSnapshot::merge(snap_a, snap_b);
    Hash top = chainsync::record_merge(cache_, tip_of(c),
                                       MergeSnapshot::leaf(c.honest_ref()),
                                       tip_of(a), composite);
    EXPECT_EQ(cache_.leaf_count(), 3u);          // + only C's own leaf
    EXPECT_EQ(cache_.composition_count(), 2u);
    // ...but the DAG stays connected: A is provable against the new top.
    auto proof = cache_.build_proof(top, MerkleTree::leaf_hash(a.honest_ref()));
    ASSERT_TRUE(proof.has_value());
    EXPECT_EQ(proof->merkle_path.path.size(), 2u);
    EXPECT_EQ(FraudProof::verify_bad_sig(top, *proof), FraudVerdict::REFUTED_HONEST);
}
