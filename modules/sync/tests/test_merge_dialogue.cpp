#include "user_ctx.h"

#include <gtest/gtest.h>

using namespace blockchain;
using chainsync::MergeDialogue;
using sync_tests::LEAF;
using sync_tests::UserCtx;
using sync_tests::pump;

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

TEST_F(MergeDialogueTest, FourChainsPairwiseMergeLinkIntoOneDag) {
    auto carol = std::make_unique<UserCtx>(base_dir_, 3);
    auto dave  = std::make_unique<UserCtx>(base_dir_, 4);
    alice_->append_block(0xAA, 1'000LL);
    bob_->append_block(0xBB, 1'000LL);
    carol->append_block(0xCC, 1'000LL);
    dave->append_block(0xDD, 1'000LL);
    Block alice_b0 = alice_->bc->get_block({alice_->root_kp.pub, LEAF, 0});
    Block bob_b0   = bob_->bc->get_block({bob_->root_kp.pub, LEAF, 0});
    Block carol_b0 = carol->bc->get_block({carol->root_kp.pub, LEAF, 0});
    Block dave_b0  = dave->bc->get_block({dave->root_kp.pub, LEAF, 0});

    // Round 1: two independent pairs.
    MergeDialogue a1 = alice_->dialogue(2'000LL);
    MergeDialogue b1 = bob_->dialogue(2'000LL);
    pump(a1, b1);
    ASSERT_TRUE(a1.done()) << a1.error();
    Hash r_ab = alice_->committed_root(*a1.merge_block());

    MergeDialogue c1 = carol->dialogue(2'000LL);
    MergeDialogue d1 = dave->dialogue(2'000LL);
    pump(c1, d1);
    ASSERT_TRUE(c1.done()) << c1.error();
    Hash r_cd = carol->committed_root(*c1.merge_block());

    // Round 2: both sides now hold composite snapshots.
    MergeDialogue a2 = alice_->dialogue(3'000LL);
    MergeDialogue c2 = carol->dialogue(3'000LL);
    pump(a2, c2);
    ASSERT_TRUE(a2.done()) << a2.error();
    ASSERT_TRUE(c2.done()) << c2.error();
    Hash top = alice_->committed_root(*a2.merge_block());
    EXPECT_EQ(top, carol->committed_root(*c2.merge_block()));

    // The top root composes the two round-1 roots: all four chains are now
    // linked into one DAG by hashes.
    auto comp = alice_->cache.get_composition(top);
    ASSERT_TRUE(comp.has_value());
    EXPECT_TRUE((comp->left_child == r_ab && comp->right_child == r_cd) ||
                (comp->left_child == r_cd && comp->right_child == r_ab));

    // Each side of round 1 cached both leaves of its pair; round 2 added only
    // the composition — a composite snapshot does not reveal its leaves (§5.2).
    EXPECT_EQ(alice_->cache.leaf_count(), 2u);
    EXPECT_EQ(alice_->cache.composition_count(), 2u);
    EXPECT_EQ(bob_->cache.leaf_count(), 2u);
    EXPECT_EQ(bob_->cache.composition_count(), 1u);
    EXPECT_EQ(carol->cache.leaf_count(), 2u);
    EXPECT_EQ(carol->cache.composition_count(), 2u);
    EXPECT_EQ(dave->cache.leaf_count(), 2u);
    EXPECT_EQ(dave->cache.composition_count(), 1u);

    // Alice proves her own pair against the top root; Carol's half must
    // arrive via gossip (§7) before she can prove those leaves.
    for (const Block* b0 : {&alice_b0, &bob_b0}) {
        Hash leaf = MerkleTree::leaf_hash(alice_->leaf_ref_of(*b0));
        auto proof = alice_->cache.build_proof(top, leaf);
        ASSERT_TRUE(proof.has_value());
        EXPECT_EQ(proof->merkle_path.path.size(), 2u);
        EXPECT_TRUE(MerkleTree::verify(leaf, proof->merkle_path, top));
        EXPECT_EQ(FraudProof::verify_bad_sig(top, *proof),
                  FraudVerdict::REFUTED_HONEST);
    }
    for (const Block* b0 : {&carol_b0, &dave_b0}) {
        Hash leaf = MerkleTree::leaf_hash(alice_->leaf_ref_of(*b0));
        EXPECT_FALSE(alice_->cache.build_proof(top, leaf).has_value());
    }
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
