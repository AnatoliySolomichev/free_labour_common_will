#include "sync/dialogue_channel.h"
#include "user_ctx.h"

#include <aggregator/server.h>
#include <blockchain/errors.h>

#include <gtest/gtest.h>
#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <thread>

using namespace blockchain;
using chainsync::DialoguePump;
using chainsync::IDialogueChannel;
using chainsync::MergeDialogue;
using chainsync::RelayEnvelope;
using chainsync::SessionId;
using chainsync::decode_relay_envelope;
using chainsync::encode_relay_envelope;
using chainsync::make_session_id;
using sync_tests::UserCtx;

// ── In-memory relay: one mailbox per user, poll drains ────────────────────────

struct InMemoryRelay {
    std::map<UserId, std::vector<RelayEnvelope>> boxes;

    class Channel : public IDialogueChannel {
    public:
        Channel(InMemoryRelay& relay, UserId self) : relay_(relay), self_(self) {}

        bool send(const UserId& to, const RelayEnvelope& env) override {
            relay_.boxes[to].push_back(env);
            return true;
        }

        std::vector<RelayEnvelope> poll() override {
            auto out = std::move(relay_.boxes[self_]);
            relay_.boxes[self_].clear();
            return out;
        }

    private:
        InMemoryRelay& relay_;
        UserId         self_;
    };

    Channel channel_for(const UserId& uid) { return Channel(*this, uid); }
};

// Runs polls on both sides until the merge settles or rounds run out.
static void pump_over_relay(DialoguePump& a, IDialogueChannel& ch_a,
                            DialoguePump& b, IDialogueChannel& ch_b) {
    for (int round = 0; round < 10 && !(a.finished() && b.finished()); ++round) {
        for (auto& env : ch_b.poll()) ASSERT_TRUE(b.feed(env));
        for (auto& env : ch_a.poll()) ASSERT_TRUE(a.feed(env));
    }
}

// ── Envelope codec ────────────────────────────────────────────────────────────

TEST(RelayEnvelope, RoundtripPreservesAllFields) {
    RelayEnvelope env;
    env.session = make_session_id();
    env.seq     = 1000;   // multi-byte CBOR head
    env.from.bytes.fill(0xAB);
    env.blob    = {0x01, 0x02, 0x03};

    const auto bytes   = encode_relay_envelope(env);
    const auto decoded = decode_relay_envelope(bytes.data(), bytes.size());
    EXPECT_EQ(decoded.session, env.session);
    EXPECT_EQ(decoded.seq,     env.seq);
    EXPECT_TRUE(decoded.from == env.from);
    EXPECT_EQ(decoded.blob,    env.blob);
}

TEST(RelayEnvelope, StrictDecodeRejectsMalformedBytes) {
    RelayEnvelope env;
    env.session = make_session_id();
    env.blob    = {0x42};
    auto bytes  = encode_relay_envelope(env);

    // Truncated.
    EXPECT_THROW(decode_relay_envelope(bytes.data(), bytes.size() / 2),
                 SerializationError);
    // Trailing byte.
    auto padded = bytes;
    padded.push_back(0x00);
    EXPECT_THROW(decode_relay_envelope(padded.data(), padded.size()),
                 SerializationError);
    // Unsupported version (byte right after the array head).
    auto wrong_version = bytes;
    wrong_version[1] = 0x02;
    EXPECT_THROW(decode_relay_envelope(wrong_version.data(), wrong_version.size()),
                 SerializationError);
    // Garbage.
    const std::vector<uint8_t> junk{0xDE, 0xAD, 0xBE, 0xEF};
    EXPECT_THROW(decode_relay_envelope(junk.data(), junk.size()),
                 SerializationError);
}

TEST(RelayEnvelope, SessionIdsDiffer) {
    EXPECT_NE(make_session_id(), make_session_id());
}

// ── Pump over an in-memory relay ──────────────────────────────────────────────

class DialogueChannelTest : public ::testing::Test {
protected:
    std::filesystem::path    base_dir_;
    std::unique_ptr<UserCtx> alice_;
    std::unique_ptr<UserCtx> bob_;

    void SetUp() override {
        static int cnt = 0;
        base_dir_ = std::filesystem::temp_directory_path()
                  / ("sync_channel_" + std::to_string(++cnt));
        std::filesystem::create_directories(base_dir_);
        alice_ = std::make_unique<UserCtx>(base_dir_, 1);
        bob_   = std::make_unique<UserCtx>(base_dir_, 2);
        alice_->append_block(0xAA, 1'000LL);
        bob_->append_block(0xBB, 1'000LL);
    }

    void TearDown() override {
        alice_.reset(); bob_.reset();
        std::filesystem::remove_all(base_dir_);
    }
};

TEST_F(DialogueChannelTest, FullMergeOverInMemoryRelay) {
    InMemoryRelay relay;
    auto ch_a = relay.channel_for(alice_->root_kp.pub);
    auto ch_b = relay.channel_for(bob_->root_kp.pub);

    MergeDialogue da = alice_->dialogue(2'000LL);
    MergeDialogue db = bob_->dialogue(2'000LL);
    const SessionId session = make_session_id();
    DialoguePump pa(da, ch_a, alice_->root_kp.pub, bob_->root_kp.pub, session);
    DialoguePump pb(db, ch_b, bob_->root_kp.pub, alice_->root_kp.pub, session);

    ASSERT_TRUE(pa.begin());
    pump_over_relay(pa, ch_a, pb, ch_b);

    ASSERT_TRUE(da.done()) << da.error();
    ASSERT_TRUE(db.done()) << db.error();
    EXPECT_EQ(alice_->committed_root(*da.merge_block()),
              bob_->committed_root(*db.merge_block()));
    // The dialogue fed both caches, exactly as with direct delivery (§5.2).
    EXPECT_EQ(alice_->cache.leaf_count(), 2u);
    EXPECT_EQ(bob_->cache.leaf_count(), 2u);
}

// Duplicates every envelope and reverses each poll batch: the pump must
// reorder by seq and drop duplicates for the merge to complete.
class HostileChannel : public IDialogueChannel {
public:
    HostileChannel(InMemoryRelay& relay, UserId self)
        : inner_(relay, self) {}

    bool send(const UserId& to, const RelayEnvelope& env) override {
        return inner_.send(to, env) && inner_.send(to, env);   // duplicate
    }

    std::vector<RelayEnvelope> poll() override {
        auto out = inner_.poll();
        std::reverse(out.begin(), out.end());                  // reorder
        return out;
    }

private:
    InMemoryRelay::Channel inner_;
};

TEST_F(DialogueChannelTest, MergeSurvivesDuplicatedAndReorderedDelivery) {
    InMemoryRelay relay;
    HostileChannel ch_a(relay, alice_->root_kp.pub);
    HostileChannel ch_b(relay, bob_->root_kp.pub);

    MergeDialogue da = alice_->dialogue(2'000LL);
    MergeDialogue db = bob_->dialogue(2'000LL);
    const SessionId session = make_session_id();
    DialoguePump pa(da, ch_a, alice_->root_kp.pub, bob_->root_kp.pub, session);
    DialoguePump pb(db, ch_b, bob_->root_kp.pub, alice_->root_kp.pub, session);

    ASSERT_TRUE(pa.begin());
    pump_over_relay(pa, ch_a, pb, ch_b);

    ASSERT_TRUE(da.done()) << da.error();
    ASSERT_TRUE(db.done()) << db.error();
    EXPECT_EQ(alice_->committed_root(*da.merge_block()),
              bob_->committed_root(*db.merge_block()));
}

// ── End to end: two users merge through a live aggregator relay ───────────────

TEST_F(DialogueChannelTest, FullMergeOverLiveHttpRelay) {
    const uint16_t port = 18590;
    aggregator::AggregatorStorage relay_storage(base_dir_ / "relay_db");
    aggregator::AggregatorServer  relay(relay_storage, port, {},
                                        std::chrono::seconds(3600));
    std::thread relay_thread([&] { relay.run(); });

    {   // Wait until the relay answers.
        httplib::Client probe("127.0.0.1:" + std::to_string(port));
        bool up = false;
        for (int i = 0; i < 100 && !up; ++i) {
            if (auto res = probe.Get("/stats"); res && res->status == 200) up = true;
            else std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(up) << "relay did not come up";
    }

    const std::string url = "http://127.0.0.1:" + std::to_string(port);
    chainsync::HttpDialogueChannel ch_a(url, alice_->root_kp.pub);
    chainsync::HttpDialogueChannel ch_b(url, bob_->root_kp.pub);

    MergeDialogue da = alice_->dialogue(2'000LL);
    MergeDialogue db = bob_->dialogue(2'000LL);
    const SessionId session = make_session_id();
    DialoguePump pa(da, ch_a, alice_->root_kp.pub, bob_->root_kp.pub, session);
    DialoguePump pb(db, ch_b, bob_->root_kp.pub, alice_->root_kp.pub, session);

    ASSERT_TRUE(pa.begin());
    for (int round = 0; round < 50 && !(pa.finished() && pb.finished()); ++round) {
        for (auto& env : ch_b.poll()) ASSERT_TRUE(pb.feed(env));
        for (auto& env : ch_a.poll()) ASSERT_TRUE(pa.feed(env));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    relay.stop();
    relay_thread.join();

    ASSERT_TRUE(da.done()) << da.error();
    ASSERT_TRUE(db.done()) << db.error();
    EXPECT_EQ(alice_->committed_root(*da.merge_block()),
              bob_->committed_root(*db.merge_block()));
    EXPECT_EQ(alice_->cache.leaf_count(), 2u);
    EXPECT_EQ(bob_->cache.leaf_count(), 2u);
    // Partner data really crossed the wire.
    EXPECT_TRUE(bob_->storage->has_node(alice_->root_kp.pub, sync_tests::LEAF));
}

TEST_F(DialogueChannelTest, ForeignSessionAndSenderAreIgnored) {
    InMemoryRelay relay;
    auto ch_b = relay.channel_for(bob_->root_kp.pub);

    MergeDialogue db = bob_->dialogue(2'000LL);
    DialoguePump pb(db, ch_b, bob_->root_kp.pub, alice_->root_kp.pub,
                    make_session_id());

    // Right sender, wrong session.
    RelayEnvelope foreign;
    foreign.session = make_session_id();
    foreign.from    = alice_->root_kp.pub;
    foreign.blob    = {0x01};
    EXPECT_TRUE(pb.feed(foreign));

    // Right session, wrong sender.
    RelayEnvelope spoofed;
    spoofed.session = pb.session();
    spoofed.from    = bob_->root_kp.pub;
    spoofed.blob    = {0x01};
    EXPECT_TRUE(pb.feed(spoofed));

    // Neither touched the dialogue: it is still idle, not failed.
    EXPECT_EQ(db.state(), MergeDialogue::State::IDLE);
    EXPECT_FALSE(pb.finished());
}
