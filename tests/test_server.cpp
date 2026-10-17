// End-to-end tests: a real server on a real socket, driven by raw RESP bytes.

#include "emberkv/server.hpp"
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace emberkv;
using namespace emberkv::testing;
using resp::Reply;

namespace {
const net::NetworkInit g_network;  // WSAStartup on Windows, before any test runs
}

class ServerTest : public ::testing::Test {
protected:
    void SetUp() override { start({}); }

    void start(ServerConfig config) {
        if (server) server->stop();
        config.port = 0;  // let the OS pick a free port: tests can run in parallel
        if (config.io_threads == 0) config.io_threads = 2;
        server = std::make_unique<Server>(config, processor, counters);
        server->start();
    }

    TestClient connect() { return TestClient(server->port()); }

    Store store{Store::Options{.shard_count = 8}};
    ServerCounters counters;
    CommandProcessor processor{store, {.snapshot_path = std::nullopt, .counters = &counters}};
    std::unique_ptr<Server> server;
};

TEST_F(ServerTest, RequestReply) {
    TestClient c = connect();
    EXPECT_EQ(c.call({"PING"}), Reply::simple("PONG"));
    EXPECT_EQ(c.call({"SET", "k", "v"}), ok());
    EXPECT_EQ(c.call({"GET", "k"}), bulk("v"));
}

TEST_F(ServerTest, InlineCommandsLikeTelnet) {
    TestClient c = connect();
    c.send_raw("SET greeting hi\r\nGET greeting\r\n");
    EXPECT_EQ(c.read(), ok());
    EXPECT_EQ(c.read(), bulk("hi"));
}

TEST_F(ServerTest, PipelinedRequestsAnsweredInOrder) {
    TestClient c = connect();
    std::string batch;
    constexpr int kCount = 2000;
    for (int i = 0; i < kCount; ++i) {
        batch += resp::encode_command(std::vector<std::string>{"SET", "k" + std::to_string(i), std::to_string(i)});
        batch += resp::encode_command(std::vector<std::string>{"GET", "k" + std::to_string(i)});
    }
    c.send_raw(batch);  // one big write, ~100 KB
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(c.read(), ok());
        ASSERT_EQ(c.read(), bulk(std::to_string(i)));
    }
}

TEST_F(ServerTest, RequestSplitAcrossManyPackets) {
    TestClient c = connect();
    const std::string wire = resp::encode_command(std::vector<std::string>{"SET", "split", "across packets"});
    for (char byte : wire) {
        c.send_raw(std::string_view(&byte, 1));
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(c.read(), ok());
    EXPECT_EQ(c.call({"GET", "split"}), bulk("across packets"));
}

TEST_F(ServerTest, LargeValuesSurvivePartialReadsAndWrites) {
    TestClient c = connect();
    std::string big(8 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + i % 26);
    EXPECT_EQ(c.call({"SET", "big", big}), ok());
    EXPECT_EQ(c.call({"GET", "big"}), bulk(big));
}

TEST_F(ServerTest, QuitRepliesThenCloses) {
    TestClient c = connect();
    EXPECT_EQ(c.call({"QUIT"}), ok());
    EXPECT_TRUE(c.closed_by_server());
}

TEST_F(ServerTest, ProtocolErrorRepliesThenCloses) {
    TestClient c = connect();
    c.send_raw("*1\r\n$-7\r\n");
    EXPECT_TRUE(is_error(c.read(), "ERR Protocol error"));
    EXPECT_TRUE(c.closed_by_server());

    TestClient other = connect();  // the server itself is fine
    EXPECT_EQ(other.call({"PING"}), Reply::simple("PONG"));
}

TEST_F(ServerTest, ManyConcurrentClients) {
    constexpr int kClients = 16, kIncrements = 500;
    std::vector<std::jthread> threads;
    for (int t = 0; t < kClients; ++t) {
        threads.emplace_back([this] {
            TestClient c = connect();
            for (int i = 0; i < kIncrements; ++i) (void)c.call({"INCR", "shared"});
        });
    }
    threads.clear();  // join

    TestClient c = connect();
    EXPECT_EQ(c.call({"GET", "shared"}), bulk(std::to_string(kClients * kIncrements)));
    EXPECT_GE(counters.total_connections.load(), static_cast<std::uint64_t>(kClients + 1));
}

TEST_F(ServerTest, ClientLimitIsEnforced) {
    start(ServerConfig{.max_clients = 1});
    TestClient first = connect();
    EXPECT_EQ(first.call({"PING"}), Reply::simple("PONG"));

    TestClient second = connect();
    EXPECT_TRUE(is_error(second.read(), "ERR max number of clients"));
    EXPECT_TRUE(second.closed_by_server());
    EXPECT_EQ(counters.rejected_connections.load(), 1u);
}

TEST_F(ServerTest, ConnectedClientCountGoesBackDown) {
    {
        TestClient a = connect();
        TestClient b = connect();
        (void)a.call({"PING"});
        (void)b.call({"PING"});
        EXPECT_EQ(counters.connected_clients.load(), 2u);
    }
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (counters.connected_clients.load() != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    EXPECT_EQ(counters.connected_clients.load(), 0u);
}

TEST_F(ServerTest, StopsCleanlyWithClientsConnected) {
    TestClient a = connect();
    TestClient b = connect();
    (void)a.call({"PING"});
    server->stop();  // must not hang
    EXPECT_TRUE(a.closed_by_server());
    EXPECT_TRUE(b.closed_by_server());
}
