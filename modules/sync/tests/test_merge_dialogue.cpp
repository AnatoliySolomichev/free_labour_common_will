#include "sync/merge_dialogue.h"
#include "sync/participant_cache.h"

#include <blockchain/blockchain.h>
#include <blockchain/crypto.h>
#include <blockchain/serializer.h>

#include <gtest/gtest.h>
#include <deque>
#include <filesystem>
#include <map>

using namespace blockchain;
using chainsync::MergeConfig;
using chainsync::MergeDialogue;
using chainsync::ParticipantCache;

static constexpr NodeIndex LEAF = 0x7FFF'FFFFu;

// ── Per-user context: chain + cache + dialogue factory ────────────────────────

struct UserCtx {
    std::filesystem::path         db_path;
    std::unique_ptr<LmdbStorage>  storage;
    std::unique_ptr<Validator>    validator;
    std::unique_ptr<Blockchain>   bc;
    std::unique_ptr<MergeSession> ms;
    ParticipantCache              cache;

    KeyPair                      root_kp;
    KeyPair                      leaf_kp;
    std::map<NodeIndex, KeyPair> path_keys;

    UserCtx(const std::filesystem::path& base, int id) {
        db_path = base / ("user_" + std::to_string(id));
        std::filesystem::remove_all(db_path);
        storage   = std::make_unique<LmdbStorage>(db_path);
        validator = std::make_unique<Validator>(*storage);
        bc        = std::make_unique<Blockchain>(*storage, *validator);
        ms        = std::make_unique<MergeSession>(*storage, *validator);

        root_kp      = Crypto::generate_keypair();
        path_keys[0] = root_kp;
        bc->create_identity(root_kp);
        for (NodeIndex idx : path_indices(LEAF))
            if (path_keys.find(idx) == path_keys.end())
                path_keys[idx] = Crypto::generate_keypair();
        bc->ensure_path(root_kp.pub, LEAF,
                        [&](NodeIndex i) { return path_keys.at(i); });
        leaf_kp = path_keys.at(LEAF);
    }

    ~UserCtx() { ms.reset(); bc.reset(); validator.reset(); storage.reset(); }

    void append_block(uint8_t seed, Timestamp ts) {
        bc->append_data_block(root_kp.pub, LEAF, {seed}, leaf_kp, ts);
    }

    MergeDialogue dialogue(Timestamp ts) {
        return MergeDialogue(*ms, cache,
                             MergeConfig{root_kp.pub, LEAF, leaf_kp, ts, 1u});
    }

    // Committed root of the branch's latest MERGE block payload.
    Hash committed_root(const Block& merge_block) const {
        MergePayload mp = Serializer::decode_merge_payload(
            merge_block.payload.data(), merge_block.payload.size());
        return mp.merkle_root;
    }

    ExternalRef leaf_ref_of(const Block& block) const {
        return ExternalRef{block.address, Crypto::hash_block(block)};
    }
};

// Delivers all pending messages between the two dialogues until both go quiet.
static void pump(MergeDialogue& initiator, MergeDialogue& responder) {
    std::deque<std::vector<uint8_t>> to_responder, to_initiator;
    for (auto& m : initiator.start()) to_responder.push_back(std::move(m));
    while (!to_responder.empty() || !to_initiator.empty()) {
        if (!to_responder.empty()) {
            auto msg = std::move(to_responder.front());
            to_responder.pop_front();
            for (auto& r : responder.on_message(msg.data(), msg.size()))
                to_initiator.push_back(std::move(r));
        } else {
            auto msg = std::move(to_initiator.front());
            to_initiator.pop_front();
            for (auto& r : initiator.on_message(msg.data(), msg.size()))
                to_responder.push_back(std::move(r));
        }
    }
}

class MergeDialogueTest : public ::testing::Test {
protected:
    std::filesystem::path    base_dir_;
    std::unique_ptr<UserCtx> alice_;
    std::unique_ptr<UserCtx> bob_;

    void SetUp() override {
        static int cnt = 0;
        base_dir_ = std::filesystem::temp_directory_path()
                  / ("sync_dialogue_" + std::to_string(++cnt));
        std::filesystem::create_directories(base_dir_);
        alice_ = std::make_unique<UserCtx>(base_dir_, 1);
        bob_   = std::make_unique<UserCtx>(base_dir_, 2);
    }

    void TearDown() override {
        alice_.reset(); bob_.reset();
        std::filesystem::remove_all(base_dir_);
    }
};

// ── Happy path ────────────────────────────────────────────────────────────────

TEST_F(MergeDialogueTest, FullMergeFillsBothCaches) {
    alice_->append_block(0xAA, 1'000LL);
    bob_->append_block(0xBB, 1'000LL);
    Block alice_b0 = alice_->bc->get_block({alice_->root_kp.pub, LEAF, 0});
    Block bob_b0   = bob_->bc->get_block({bob_->root_kp.pub, LEAF, 0});

    MergeDialogue a = alice_->dialogue(2'000LL);
    MergeDialogue b = bob_->dialogue(2'000LL);
    pump(a, b);

    ASSERT_TRUE(a.done()) << a.error();
    ASSERT_TRUE(b.done()) << b.error();
    ASSERT_TRUE(a.merge_block().has_value());
    ASSERT_TRUE(b.merge_block().has_value());
    EXPECT_TRUE(a.merge_block()->co_signature.has_value());
    EXPECT_TRUE(b.merge_block()->co_signature.has_value());

    // Both sides committed the same union root.
    Hash root = alice_->committed_root(*a.merge_block());
    EXPECT_EQ(root, bob_->committed_root(*b.merge_block()));

    // Partner data imported on both sides.
    EXPECT_TRUE(bob_->storage->has_node(alice_->root_kp.pub, LEAF));
    EXPECT_TRUE(alice_->storage->has_external_block(bob_b0.address));

    // §5.2: each cache holds both single-leaf records + the composition, and
    // can prove either participant against the committed root.
    for (UserCtx* u : {alice_.get(), bob_.get()}) {
        EXPECT_EQ(u->cache.leaf_count(), 2u);
        EXPECT_EQ(u->cache.composition_count(), 1u);
        for (const Block* b0 : {&alice_b0, &bob_b0}) {
            ExternalRef ref = u->leaf_ref_of(*b0);
            Hash leaf = MerkleTree::leaf_hash(ref);
            auto proof = u->cache.build_proof(root, leaf);
            ASSERT_TRUE(proof.has_value());
            EXPECT_TRUE(MerkleTree::verify(leaf, proof->merkle_path, root));
            // Honest chains → structurally sound proof, defect absent.
            EXPECT_EQ(FraudProof::verify_bad_sig(root, *proof),
                      FraudVerdict::REFUTED_HONEST);
        }
    }
}

TEST_F(MergeDialogueTest, SecondMergeGrowsDagAndDeepensProofs) {
    auto carol = std::make_unique<UserCtx>(base_dir_, 3);
    alice_->append_block(0xAA, 1'000LL);
    bob_->append_block(0xBB, 1'000LL);
    carol->append_block(0xCC, 1'000LL);
    Block bob_b0 = bob_->bc->get_block({bob_->root_kp.pub, LEAF, 0});

    MergeDialogue a1 = alice_->dialogue(2'000LL);
    MergeDialogue b1 = bob_->dialogue(2'000LL);
    pump(a1, b1);
    ASSERT_TRUE(a1.done()) << a1.error();

    MergeDialogue a2 = alice_->dialogue(3'000LL);
    MergeDialogue c2 = carol->dialogue(3'000LL);
    pump(a2, c2);
    ASSERT_TRUE(a2.done()) << a2.error();
    ASSERT_TRUE(c2.done()) << c2.error();

    // Alice saw everything: 3 leaves, 2 compositions; Bob's leaf (from the
    // first merge) is provable against the second merge's committed root
    // through a depth-2 path.
    EXPECT_EQ(alice_->cache.leaf_count(), 3u);
    EXPECT_EQ(alice_->cache.composition_count(), 2u);
    Hash top = alice_->committed_root(*a2.merge_block());
    Hash bob_leaf = MerkleTree::leaf_hash(alice_->leaf_ref_of(bob_b0));
    auto proof = alice_->cache.build_proof(top, bob_leaf);
    ASSERT_TRUE(proof.has_value());
    EXPECT_EQ(proof->merkle_path.path.size(), 2u);
    EXPECT_EQ(FraudProof::verify_bad_sig(top, *proof), FraudVerdict::REFUTED_HONEST);

    // Carol met Alice's already-composite snapshot: she can cache only her own
    // leaf and the composition — Bob's leaf must come via gossip later (§7).
    EXPECT_EQ(carol->cache.leaf_count(), 1u);
    EXPECT_EQ(carol->cache.composition_count(), 1u);
    EXPECT_FALSE(carol->cache.build_proof(top, bob_leaf).has_value());
}

// ── Message flow shape (documents the wire sequence) ──────────────────────────

TEST_F(MergeDialogueTest, MessageFlowSequence) {
    alice_->append_block(0x01, 1'000LL);
    bob_->append_block(0x02, 1'000LL);

    MergeDialogue a = alice_->dialogue(2'000LL);
    MergeDialogue b = bob_->dialogue(2'000LL);

    auto offer = a.start();
    ASSERT_EQ(offer.size(), 1u);                          // OFFER

    auto accept_draft = b.on_message(offer[0].data(), offer[0].size());
    ASSERT_EQ(accept_draft.size(), 2u);                   // ACCEPT + DRAFT

    auto draft_a = a.on_message(accept_draft[0].data(), accept_draft[0].size());
    ASSERT_EQ(draft_a.size(), 1u);                        // DRAFT

    auto cosig_a = a.on_message(accept_draft[1].data(), accept_draft[1].size());
    ASSERT_EQ(cosig_a.size(), 1u);                        // COSIG for Bob

    auto cosig_b = b.on_message(draft_a[0].data(), draft_a[0].size());
    ASSERT_EQ(cosig_b.size(), 1u);                        // COSIG for Alice

    EXPECT_TRUE(b.on_message(cosig_a[0].data(), cosig_a[0].size()).empty());
    EXPECT_TRUE(a.on_message(cosig_b[0].data(), cosig_b[0].size()).empty());
    EXPECT_TRUE(a.done());
    EXPECT_TRUE(b.done());

    // Messages after DONE are ignored, the dialogue stays completed.
    EXPECT_TRUE(a.on_message(offer[0].data(), offer[0].size()).empty());
    EXPECT_TRUE(a.done());
}

// ── Failure paths ─────────────────────────────────────────────────────────────

TEST_F(MergeDialogueTest, EmptyOwnBranchFailsToStart) {
    bob_->append_block(0x02, 1'000LL);
    MergeDialogue a = alice_->dialogue(2'000LL);   // Alice has no blocks
    EXPECT_TRUE(a.start().empty());
    EXPECT_TRUE(a.failed());
    EXPECT_FALSE(a.error().empty());
}

TEST_F(MergeDialogueTest, EmptyResponderBranchFailsAndInitiatorStalls) {
    alice_->append_block(0x01, 1'000LL);           // Bob has no blocks
    MergeDialogue a = alice_->dialogue(2'000LL);
    MergeDialogue b = bob_->dialogue(2'000LL);
    pump(a, b);
    EXPECT_TRUE(b.failed());
    // Alice never hears back — a stalled attempt, not a failure (§11.4).
    EXPECT_FALSE(a.done());
    EXPECT_FALSE(a.failed());
    EXPECT_EQ(a.state(), MergeDialogue::State::WAIT_ACCEPT);
}

TEST_F(MergeDialogueTest, GarbageAndTamperedMessagesFail) {
    alice_->append_block(0x01, 1'000LL);
    bob_->append_block(0x02, 1'000LL);

    {   // junk bytes
        MergeDialogue b = bob_->dialogue(2'000LL);
        const std::vector<uint8_t> junk{0x00, 0x01, 0x02};
        EXPECT_TRUE(b.on_message(junk.data(), junk.size()).empty());
        EXPECT_TRUE(b.failed());
    }
    {   // tampered OFFER (corrupt a byte inside the payload)
        MergeDialogue a = alice_->dialogue(2'000LL);
        MergeDialogue b = bob_->dialogue(2'000LL);
        auto offer = a.start();
        ASSERT_EQ(offer.size(), 1u);
        offer[0][offer[0].size() / 2] ^= 0xFF;
        b.on_message(offer[0].data(), offer[0].size());
        EXPECT_TRUE(b.failed());
    }
}

TEST_F(MergeDialogueTest, OfferToAnotherInitiatorFails) {
    alice_->append_block(0x01, 1'000LL);
    bob_->append_block(0x02, 1'000LL);
    MergeDialogue a = alice_->dialogue(2'000LL);
    MergeDialogue b = bob_->dialogue(2'000LL);
    auto offer_a = a.start();
    auto offer_b = b.start();
    ASSERT_EQ(offer_b.size(), 1u);
    // Both sides initiated: an OFFER is not valid in WAIT_ACCEPT.
    a.on_message(offer_b[0].data(), offer_b[0].size());
    EXPECT_TRUE(a.failed());
}

TEST_F(MergeDialogueTest, TamperedCoSignatureFails) {
    alice_->append_block(0x01, 1'000LL);
    bob_->append_block(0x02, 1'000LL);
    MergeDialogue a = alice_->dialogue(2'000LL);
    MergeDialogue b = bob_->dialogue(2'000LL);

    auto offer        = a.start();
    auto accept_draft = b.on_message(offer[0].data(), offer[0].size());
    auto draft_a      = a.on_message(accept_draft[0].data(), accept_draft[0].size());
    a.on_message(accept_draft[1].data(), accept_draft[1].size());
    auto cosig_b      = b.on_message(draft_a[0].data(), draft_a[0].size());
    ASSERT_EQ(cosig_b.size(), 1u);

    cosig_b[0].back() ^= 0xFF;   // corrupt the co-signature bytes
    a.on_message(cosig_b[0].data(), cosig_b[0].size());
    EXPECT_TRUE(a.failed());
    EXPECT_FALSE(a.merge_block().has_value());
}
