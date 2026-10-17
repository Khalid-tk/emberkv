#include "emberkv/config.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string_view>
#include <vector>

using namespace emberkv;

namespace {
Config parse(std::vector<std::string_view> args) { return parse_args(args); }
}  // namespace

TEST(Config, ParseSize) {
    EXPECT_EQ(parse_size("1024"), 1024u);
    EXPECT_EQ(parse_size("4k"), 4096u);
    EXPECT_EQ(parse_size("64mb"), 64u << 20);
    EXPECT_EQ(parse_size("2GB"), std::size_t{2} << 30);
    EXPECT_EQ(parse_size("12b"), 12u);
    EXPECT_EQ(parse_size(""), std::nullopt);
    EXPECT_EQ(parse_size("mb"), std::nullopt);
    EXPECT_EQ(parse_size("10xb"), std::nullopt);
    EXPECT_EQ(parse_size("-1"), std::nullopt);
    EXPECT_EQ(parse_size("99999999999999999999g"), std::nullopt);
}

TEST(Config, Defaults) {
    const Config c = parse({});
    EXPECT_EQ(c.server.port, 6380);
    EXPECT_EQ(c.server.bind, "127.0.0.1");
    EXPECT_FALSE(c.snapshot);
}

TEST(Config, AllOptions) {
    const Config c = parse({"--bind", "0.0.0.0", "-p", "7000", "--threads", "4", "--shards", "32", "--maxmemory",
                            "1gb", "--maxclients", "50", "--snapshot", "data.ekv"});
    EXPECT_EQ(c.server.bind, "0.0.0.0");
    EXPECT_EQ(c.server.port, 7000);
    EXPECT_EQ(c.server.io_threads, 4u);
    EXPECT_EQ(c.shards, 32u);
    EXPECT_EQ(c.max_memory, std::size_t{1} << 30);
    EXPECT_EQ(c.server.max_clients, 50u);
    EXPECT_EQ(c.snapshot, std::filesystem::path("data.ekv"));
}

TEST(Config, RejectsBadInput) {
    EXPECT_THROW(parse({"--port"}), std::invalid_argument);
    EXPECT_THROW(parse({"--port", "70000"}), std::invalid_argument);
    EXPECT_THROW(parse({"--port", "http"}), std::invalid_argument);
    EXPECT_THROW(parse({"--threads", "0"}), std::invalid_argument);
    EXPECT_THROW(parse({"--maxmemory", "10k"}), std::invalid_argument);
    EXPECT_THROW(parse({"--frobnicate"}), std::invalid_argument);
}
