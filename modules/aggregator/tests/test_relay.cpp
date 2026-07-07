#include "aggregator/server.h"

#include <gtest/gtest.h>
#include <httplib.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

using namespace aggregator;

// ── Live-server fixture ───────────────────────────────────────────────────────

namespace {

constexpr const char* kUidA = // 64 hex chars
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr const char* kUidB =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

uint16_t next_port() {
    static uint16_t port = 18470;
    return ++port;
}

} // namespace

class RelayTest : public ::testing::Test {
protected:
    std::filesystem::path              db_path_;
    std::unique_ptr<AggregatorStorage> storage_;
    std::unique_ptr<AggregatorServer>  server_;
    std::thread                        server_thread_;
    uint16_t                           port_ = 0;

    void start_server(std::chrono::seconds ttl) {
        static int cnt = 0;
        db_path_ = std::filesystem::temp_directory_path() /
                   ("bc_relay_test_" + std::to_string(++cnt));
        std::filesystem::remove_all(db_path_);
        storage_ = std::make_unique<AggregatorStorage>(db_path_);
        port_    = next_port();
        server_  = std::make_unique<AggregatorServer>(
            *storage_, port_, std::vector<std::string>{},
            std::chrono::seconds(3600), ttl);
        server_thread_ = std::thread([this] { server_->run(); });

        // Wait until the server answers.
        httplib::Client probe(host());
        for (int i = 0; i < 100; ++i) {
            if (auto res = probe.Get("/stats"); res && res->status == 200) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        FAIL() << "aggregator server did not come up";
    }

    void TearDown() override {
        if (server_) server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
        server_.reset();
        storage_.reset();
        std::filesystem::remove_all(db_path_);
    }

    std::string host() const { return "127.0.0.1:" + std::to_string(port_); }

    int post(httplib::Client& cli, const std::string& uid, const std::string& body) {
        auto res = cli.Post("/dialogue/" + uid, body, "application/cbor");
        return res ? res->status : -1;
    }

    std::string drain(httplib::Client& cli, const std::string& uid) {
        auto res = cli.Get("/dialogue/" + uid + "/inbox");
        EXPECT_TRUE(res && res->status == 200);
        return res ? res->body : "";
    }
};

// ── Tests ─────────────────────────────────────────────────────────────────────

TEST_F(RelayTest, PostThenDrainReturnsBytesVerbatimAndEmptiesBox) {
    start_server(std::chrono::seconds(3600));
    httplib::Client cli(host());

    // Envelopes are opaque to the relay — arbitrary bytes must round-trip.
    const std::string e1("\x01\x02\xFF junk-1", 10);
    const std::string e2("\x00 junk-2", 8);
    EXPECT_EQ(post(cli, kUidA, e1), 200);
    EXPECT_EQ(post(cli, kUidA, e2), 200);

    // CBOR: array(2), then each entry as bstr, in arrival order.
    std::string body = drain(cli, kUidA);
    std::string expected;
    expected += static_cast<char>(0x82);            // array(2)
    expected += static_cast<char>(0x4A); expected += e1;   // bstr(10)
    expected += static_cast<char>(0x48); expected += e2;   // bstr(8)
    EXPECT_EQ(body, expected);

    // Drained: next poll is an empty array.
    EXPECT_EQ(drain(cli, kUidA), std::string(1, static_cast<char>(0x80)));
    // Another user's box is untouched by all of the above.
    EXPECT_EQ(drain(cli, kUidB), std::string(1, static_cast<char>(0x80)));
}

TEST_F(RelayTest, DuplicatePostStoredOnce) {
    start_server(std::chrono::seconds(3600));
    httplib::Client cli(host());

    const std::string env = "same-envelope";
    EXPECT_EQ(post(cli, kUidA, env), 200);
    EXPECT_EQ(post(cli, kUidA, env), 200);   // idempotent retry

    std::string body = drain(cli, kUidA);
    EXPECT_EQ(static_cast<uint8_t>(body[0]), 0x81u);   // array(1)
}

TEST_F(RelayTest, InvalidUidRejected) {
    start_server(std::chrono::seconds(3600));
    httplib::Client cli(host());

    EXPECT_EQ(post(cli, "not-hex", "x"), 400);
    auto res = cli.Get("/dialogue/short/inbox");
    ASSERT_TRUE(res);
    EXPECT_EQ(res->status, 400);
}

TEST_F(RelayTest, ExpiredEnvelopesArePruned) {
    start_server(std::chrono::seconds(0));   // everything expires immediately
    httplib::Client cli(host());

    EXPECT_EQ(post(cli, kUidA, "gone"), 200);
    EXPECT_EQ(drain(cli, kUidA), std::string(1, static_cast<char>(0x80)));
}

// ── Snapshot warehouse (sync.md §7.1) ─────────────────────────────────────────

TEST_F(RelayTest, WarehouseRoundtripFirstWriteWins) {
    start_server(std::chrono::seconds(3600));
    httplib::Client cli(host());

    const std::string key(64, 'a');
    const std::string leaf_bytes = "opaque-leaf-bytes";

    auto post = cli.Post("/snapshot/leaf/" + key, leaf_bytes, "application/octet-stream");
    ASSERT_TRUE(post && post->status == 200);
    EXPECT_NE(post->body.find("stored"), std::string::npos);

    // Second write for the same key is refused (first write wins).
    auto post2 = cli.Post("/snapshot/leaf/" + key, "displacement attempt",
                          "application/octet-stream");
    ASSERT_TRUE(post2 && post2->status == 200);
    EXPECT_NE(post2->body.find("duplicate"), std::string::npos);

    auto got = cli.Get("/snapshot/leaf/" + key);
    ASSERT_TRUE(got && got->status == 200);
    EXPECT_EQ(got->body, leaf_bytes);                       // verbatim, original

    // Compositions must be exactly 64 bytes.
    const std::string ckey(64, 'b');
    auto bad = cli.Post("/snapshot/composition/" + ckey, "short",
                        "application/octet-stream");
    ASSERT_TRUE(bad);
    EXPECT_EQ(bad->status, 400);
    auto ok = cli.Post("/snapshot/composition/" + ckey, std::string(64, '\x07'),
                       "application/octet-stream");
    ASSERT_TRUE(ok && ok->status == 200);

    // Manifests list the stored keys.
    auto lm = cli.Get("/snapshot/leaves/manifest");
    ASSERT_TRUE(lm && lm->status == 200);
    EXPECT_NE(lm->body.find(key), std::string::npos);
    auto cm = cli.Get("/snapshot/compositions/manifest");
    ASSERT_TRUE(cm && cm->status == 200);
    EXPECT_NE(cm->body.find(ckey), std::string::npos);

    auto missing = cli.Get("/snapshot/leaf/" + std::string(64, 'f'));
    ASSERT_TRUE(missing);
    EXPECT_EQ(missing->status, 404);
}

TEST_F(RelayTest, AggregatorsGossipWarehouseEntries) {
    start_server(std::chrono::seconds(3600));   // server A (this->server_)
    httplib::Client cli_a(host());

    const std::string lkey(64, 'c');
    const std::string ckey(64, 'd');
    ASSERT_TRUE(cli_a.Post("/snapshot/leaf/" + lkey, "gossiped-leaf",
                           "application/octet-stream"));
    ASSERT_TRUE(cli_a.Post("/snapshot/composition/" + ckey, std::string(64, '\x09'),
                           "application/octet-stream"));

    // Server B peers with A and pulls on a 1s interval.
    const uint16_t port_b = port_ + 100;
    auto db_b = db_path_;
    db_b += "_b";
    AggregatorStorage storage_b(db_b);
    AggregatorServer  server_b(storage_b, port_b,
                               {"http://127.0.0.1:" + std::to_string(port_)},
                               std::chrono::seconds(1));
    std::thread thread_b([&] { server_b.run(); });

    httplib::Client cli_b("127.0.0.1:" + std::to_string(port_b));
    bool arrived = false;
    for (int i = 0; i < 100 && !arrived; ++i) {
        auto res = cli_b.Get("/snapshot/leaf/" + lkey);
        if (res && res->status == 200 && res->body == "gossiped-leaf") arrived = true;
        else std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server_b.stop();
    thread_b.join();
    std::filesystem::remove_all(db_b);

    EXPECT_TRUE(arrived) << "warehouse entry did not gossip within 10s";
}

TEST_F(RelayTest, FullMailboxAnswers429) {
    start_server(std::chrono::seconds(3600));
    httplib::Client cli(host());

    for (std::size_t i = 0; i < AggregatorServer::kMailboxCap; ++i)
        ASSERT_EQ(post(cli, kUidA, "env-" + std::to_string(i)), 200);
    EXPECT_EQ(post(cli, kUidA, "overflow"), 429);
    // The other mailbox still accepts (per-recipient quota).
    EXPECT_EQ(post(cli, kUidB, "fine"), 200);
}
