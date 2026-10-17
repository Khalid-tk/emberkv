#include "emberkv/commands.hpp"
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <string>

using namespace emberkv;
using namespace emberkv::testing;
using resp::Reply;

class Commands : public ::testing::Test {
protected:
    Reply run(std::initializer_list<std::string> args) { return processor.execute(std::vector<std::string>(args)); }

    ManualClock clock;
    Store store{Store::Options{.shard_count = 4, .max_memory = 64u * 1024 * 1024, .clock = clock.function()}};
    CommandProcessor processor{store};
};

TEST_F(Commands, PingAndEcho) {
    EXPECT_EQ(run({"PING"}), Reply::simple("PONG"));
    EXPECT_EQ(run({"ping", "hi"}), bulk("hi"));
    EXPECT_EQ(run({"ECHO", "x y"}), bulk("x y"));
}

TEST_F(Commands, NamesAreCaseInsensitive) {
    EXPECT_EQ(run({"sEt", "k", "v"}), ok());
    EXPECT_EQ(run({"get", "k"}), bulk("v"));
}

TEST_F(Commands, UnknownCommandAndWrongArity) {
    EXPECT_TRUE(is_error(run({"NOPE"}), "ERR unknown command"));
    EXPECT_EQ(run({"GET"}), Reply::error("ERR wrong number of arguments for 'get' command"));
    EXPECT_TRUE(is_error(run({"GET", "a", "b"}), "ERR wrong number"));
    EXPECT_TRUE(is_error(run({"MSET", "a", "1", "b"}), "ERR wrong number"));
}

// ------------------------------------------------------------------ strings

TEST_F(Commands, SetGetAndOverwrite) {
    EXPECT_EQ(run({"GET", "k"}), null());
    EXPECT_EQ(run({"SET", "k", "v1"}), ok());
    EXPECT_EQ(run({"SET", "k", "v2"}), ok());
    EXPECT_EQ(run({"GET", "k"}), bulk("v2"));
}

TEST_F(Commands, SetNxXx) {
    EXPECT_EQ(run({"SET", "k", "a", "XX"}), null());
    EXPECT_EQ(run({"SET", "k", "a", "NX"}), ok());
    EXPECT_EQ(run({"SET", "k", "b", "NX"}), null());
    EXPECT_EQ(run({"SET", "k", "c", "XX"}), ok());
    EXPECT_EQ(run({"GET", "k"}), bulk("c"));
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "NX", "XX"}), "ERR syntax"));
}

TEST_F(Commands, SetWithExpiry) {
    EXPECT_EQ(run({"SET", "k", "v", "PX", "100"}), ok());
    clock.advance(99ms);
    EXPECT_EQ(run({"GET", "k"}), bulk("v"));
    clock.advance(1ms);
    EXPECT_EQ(run({"GET", "k"}), null());

    EXPECT_EQ(run({"SET", "s", "v", "EX", "10"}), ok());
    EXPECT_EQ(run({"TTL", "s"}), integer(10));
}

TEST_F(Commands, SetKeepTtlAndGetOption) {
    run({"SET", "k", "old", "EX", "100"});
    EXPECT_EQ(run({"SET", "k", "new", "KEEPTTL", "GET"}), bulk("old"));
    EXPECT_EQ(run({"TTL", "k"}), integer(100));
    EXPECT_EQ(run({"SET", "k", "newer"}), ok());
    EXPECT_EQ(run({"TTL", "k"}), integer(-1)) << "a plain SET clears the TTL";
}

TEST_F(Commands, SetRejectsBadOptions) {
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "EX"}), "ERR syntax"));
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "EX", "0"}), "ERR invalid expire"));
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "EX", "-5"}), "ERR invalid expire"));
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "EX", "soon"}), "ERR value is not an integer"));
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "EX", "99999999999999999"}), "ERR invalid expire"));
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "EX", "1", "PX", "1"}), "ERR syntax"));
    EXPECT_TRUE(is_error(run({"SET", "k", "v", "BOGUS"}), "ERR syntax"));
}

TEST_F(Commands, IncrementFamily) {
    EXPECT_EQ(run({"INCR", "n"}), integer(1));
    EXPECT_EQ(run({"INCRBY", "n", "41"}), integer(42));
    EXPECT_EQ(run({"DECR", "n"}), integer(41));
    EXPECT_EQ(run({"DECRBY", "n", "50"}), integer(-9));
    EXPECT_EQ(run({"GET", "n"}), bulk("-9"));
    EXPECT_EQ(run({"DECRBY", "fresh", "5"}), integer(-5));
}

TEST_F(Commands, IncrementRejectsNonIntegersAndOverflow) {
    run({"SET", "s", "abc"});
    EXPECT_TRUE(is_error(run({"INCR", "s"}), "ERR value is not an integer"));
    run({"SET", "f", "1.5"});
    EXPECT_TRUE(is_error(run({"INCR", "f"}), "ERR value is not an integer"));
    EXPECT_TRUE(is_error(run({"INCRBY", "n", "x"}), "ERR value is not an integer"));

    run({"SET", "max", std::to_string(std::numeric_limits<std::int64_t>::max())});
    EXPECT_TRUE(is_error(run({"INCR", "max"}), "ERR increment or decrement would overflow"));
    run({"SET", "min", std::to_string(std::numeric_limits<std::int64_t>::min())});
    EXPECT_TRUE(is_error(run({"DECR", "min"}), "ERR increment or decrement would overflow"));
    EXPECT_TRUE(is_error(run({"DECRBY", "x", "-9223372036854775808"}), "ERR decrement would overflow"));
}

TEST_F(Commands, IncrementKeepsTheTtl) {
    run({"SET", "n", "1", "EX", "50"});
    run({"INCR", "n"});
    EXPECT_EQ(run({"TTL", "n"}), integer(50));
}

TEST_F(Commands, AppendStrlenGetdel) {
    EXPECT_EQ(run({"APPEND", "s", "Hello"}), integer(5));
    EXPECT_EQ(run({"APPEND", "s", " World"}), integer(11));
    EXPECT_EQ(run({"STRLEN", "s"}), integer(11));
    EXPECT_EQ(run({"STRLEN", "missing"}), integer(0));
    EXPECT_EQ(run({"GETDEL", "s"}), bulk("Hello World"));
    EXPECT_EQ(run({"EXISTS", "s"}), integer(0));
}

TEST_F(Commands, MsetMget) {
    EXPECT_EQ(run({"MSET", "a", "1", "b", "2", "c", "3"}), ok());
    run({"RPUSH", "list", "x"});
    EXPECT_EQ(run({"MGET", "a", "missing", "c", "list"}), array({bulk("1"), null(), bulk("3"), null()}));
}

// ------------------------------------------------------------------ keyspace

TEST_F(Commands, DelAndExists) {
    run({"MSET", "a", "1", "b", "2"});
    EXPECT_EQ(run({"EXISTS", "a", "a", "b", "nope"}), integer(3)) << "repeated keys count each time, as in Redis";
    EXPECT_EQ(run({"DEL", "a", "nope", "b"}), integer(2));
    EXPECT_EQ(run({"EXISTS", "a", "b"}), integer(0));
}

TEST_F(Commands, ExpiredKeysAreNotCountedByDel) {
    run({"SET", "k", "v", "PX", "5"});
    clock.advance(5ms);
    EXPECT_EQ(run({"DEL", "k"}), integer(0));
}

TEST_F(Commands, Type) {
    run({"SET", "s", "v"});
    run({"LPUSH", "l", "v"});
    EXPECT_EQ(run({"TYPE", "s"}), Reply::simple("string"));
    EXPECT_EQ(run({"TYPE", "l"}), Reply::simple("list"));
    EXPECT_EQ(run({"TYPE", "none"}), Reply::simple("none"));
}

TEST_F(Commands, KeysMatchesGlobAndSortsTheResult) {
    run({"MSET", "user:2", "b", "user:1", "a", "order:1", "c"});
    EXPECT_EQ(run({"KEYS", "user:*"}), array({bulk("user:1"), bulk("user:2")}));
    EXPECT_EQ(run({"KEYS", "*:1"}), array({bulk("order:1"), bulk("user:1")}));
    EXPECT_EQ(run({"KEYS", "nothing*"}), array({}));
}

TEST_F(Commands, ExpireTtlPersist) {
    EXPECT_EQ(run({"TTL", "missing"}), integer(-2));
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"TTL", "k"}), integer(-1));
    EXPECT_EQ(run({"EXPIRE", "k", "10"}), integer(1));
    clock.advance(2400ms);
    EXPECT_EQ(run({"TTL", "k"}), integer(8)) << "7.6s rounds to 8";
    EXPECT_EQ(run({"PTTL", "k"}), integer(7600));
    EXPECT_EQ(run({"PERSIST", "k"}), integer(1));
    EXPECT_EQ(run({"PERSIST", "k"}), integer(0));
    EXPECT_EQ(run({"TTL", "k"}), integer(-1));
    EXPECT_EQ(run({"PEXPIRE", "k", "1"}), integer(1));
    clock.advance(1ms);
    EXPECT_EQ(run({"TTL", "k"}), integer(-2));
    EXPECT_EQ(run({"EXPIRE", "missing", "10"}), integer(0));
}

TEST_F(Commands, ExpireInThePastDeletes) {
    run({"SET", "k", "v"});
    EXPECT_EQ(run({"EXPIRE", "k", "-1"}), integer(1));
    EXPECT_EQ(run({"EXISTS", "k"}), integer(0));
    EXPECT_TRUE(is_error(run({"EXPIRE", "k", "soon"}), "ERR value is not an integer"));
}

// --------------------------------------------------------------------- lists

TEST_F(Commands, PushAndRange) {
    EXPECT_EQ(run({"LPUSH", "l", "a", "b", "c"}), integer(3));  // c b a
    EXPECT_EQ(run({"RPUSH", "l", "d"}), integer(4));            // c b a d
    EXPECT_EQ(run({"LRANGE", "l", "0", "-1"}), array({bulk("c"), bulk("b"), bulk("a"), bulk("d")}));
    EXPECT_EQ(run({"LRANGE", "l", "1", "2"}), array({bulk("b"), bulk("a")}));
    EXPECT_EQ(run({"LRANGE", "l", "-2", "100"}), array({bulk("a"), bulk("d")}));
    EXPECT_EQ(run({"LRANGE", "l", "5", "10"}), array({}));
    EXPECT_EQ(run({"LRANGE", "l", "2", "1"}), array({}));
    EXPECT_EQ(run({"LRANGE", "missing", "0", "-1"}), array({}));
    EXPECT_EQ(run({"LLEN", "l"}), integer(4));
    EXPECT_TRUE(is_error(run({"LRANGE", "l", "a", "1"}), "ERR value is not an integer"));
}

TEST_F(Commands, PopRemovesEmptyLists) {
    run({"RPUSH", "l", "a", "b", "c"});
    EXPECT_EQ(run({"LPOP", "l"}), bulk("a"));
    EXPECT_EQ(run({"RPOP", "l"}), bulk("c"));
    EXPECT_EQ(run({"RPOP", "l"}), bulk("b"));
    EXPECT_EQ(run({"EXISTS", "l"}), integer(0)) << "a list that becomes empty is deleted";
    EXPECT_EQ(run({"LPOP", "l"}), null());
}

TEST_F(Commands, PopWithCount) {
    run({"RPUSH", "l", "1", "2", "3", "4"});
    EXPECT_EQ(run({"LPOP", "l", "2"}), array({bulk("1"), bulk("2")}));
    EXPECT_EQ(run({"RPOP", "l", "10"}), array({bulk("4"), bulk("3")}));
    EXPECT_TRUE(is_error(run({"LPOP", "l", "-1"}), "ERR value is out of range"));
}

TEST_F(Commands, WrongTypeErrors) {
    run({"SET", "s", "v"});
    run({"RPUSH", "l", "x"});
    EXPECT_TRUE(is_error(run({"GET", "l"}), "WRONGTYPE"));
    EXPECT_TRUE(is_error(run({"INCR", "l"}), "WRONGTYPE"));
    EXPECT_TRUE(is_error(run({"APPEND", "l", "x"}), "WRONGTYPE"));
    EXPECT_TRUE(is_error(run({"LPUSH", "s", "x"}), "WRONGTYPE"));
    EXPECT_TRUE(is_error(run({"LRANGE", "s", "0", "1"}), "WRONGTYPE"));
    EXPECT_TRUE(is_error(run({"SET", "l", "v", "GET"}), "WRONGTYPE"));
    EXPECT_EQ(run({"SET", "l", "v"}), ok()) << "a plain SET replaces any type";
}

// ---------------------------------------------------------- server commands

TEST_F(Commands, DbsizeFlushallInfo) {
    run({"MSET", "a", "1", "b", "2"});
    EXPECT_EQ(run({"DBSIZE"}), integer(2));
    const Reply info = run({"INFO"});
    const auto* text = std::get_if<std::string>(&info.value);
    ASSERT_NE(text, nullptr);
    EXPECT_NE(text->find("# Memory"), std::string::npos);
    EXPECT_NE(text->find("db0:keys=2,expires=0"), std::string::npos);

    const Reply keyspace = run({"INFO", "keyspace"});
    EXPECT_EQ(std::get<std::string>(keyspace.value).find("# Memory"), std::string::npos);

    EXPECT_EQ(run({"FLUSHALL"}), ok());
    EXPECT_EQ(run({"DBSIZE"}), integer(0));
}

TEST_F(Commands, ConfigGetForClientCompatibility) {
    EXPECT_EQ(run({"CONFIG", "GET", "appendonly"}), array({bulk("appendonly"), bulk("no")}));
    EXPECT_EQ(run({"CONFIG", "GET", "nonexistent"}), array({}));
}

TEST(CommandsMemory, OversizedValuesAreRejected) {
    Store store(Store::Options{.shard_count = 1, .max_memory = 4096});
    CommandProcessor processor(store);
    auto run = [&](std::vector<std::string> args) { return processor.execute(std::move(args)); };

    EXPECT_TRUE(is_error(run({"SET", "big", std::string(8192, 'x')}), "OOM"));
    EXPECT_EQ(run({"SET", "small", "v"}), ok());
    EXPECT_TRUE(is_error(run({"APPEND", "small", std::string(8192, 'x')}), "OOM"));
    EXPECT_EQ(run({"GET", "small"}), bulk("v")) << "a rejected APPEND leaves the value alone";
    EXPECT_TRUE(is_error(run({"MSET", "a", "1", "b", std::string(8192, 'x')}), "OOM"));
    EXPECT_EQ(run({"EXISTS", "a"}), integer(0)) << "MSET is all-or-nothing";
}

TEST(CommandsMemory, LruEvictionKeepsUsageUnderTheLimit) {
    Store store(Store::Options{.shard_count = 1, .max_memory = 64 * 1024});
    CommandProcessor processor(store);
    for (int i = 0; i < 5000; ++i) (void)processor.execute({"SET", "key:" + std::to_string(i), std::string(100, 'v')});

    const StoreStats stats = store.stats();
    EXPECT_LE(stats.used_memory, stats.max_memory);
    EXPECT_GT(stats.evictions, 0u);
    EXPECT_EQ(processor.execute({"GET", "key:4999"}), bulk(std::string(100, 'v'))) << "newest key survives";
    EXPECT_EQ(processor.execute({"GET", "key:0"}), null()) << "oldest key was evicted";
}

TEST(CommandsPersistence, SaveNeedsAConfiguredFile) {
    Store store;
    CommandProcessor processor(store);
    EXPECT_TRUE(is_error(processor.execute({"SAVE"}), "ERR no snapshot file"));
    EXPECT_TRUE(is_error(processor.execute({"BGSAVE"}), "ERR no snapshot file"));
}

TEST(CommandsPersistence, SaveAndBgsave) {
    const auto path = std::filesystem::temp_directory_path() / "emberkv-test-commands.ekv";
    std::filesystem::remove(path);
    {
        Store store;
        CommandProcessor processor(store, {.snapshot_path = path});
        (void)processor.execute({"SET", "k", "v"});
        EXPECT_EQ(processor.execute({"LASTSAVE"}), integer(0));
        EXPECT_EQ(processor.execute({"SAVE"}), ok());
        EXPECT_TRUE(std::filesystem::exists(path));
        EXPECT_EQ(processor.execute({"BGSAVE"}), Reply::simple("Background saving started"));
        processor.wait_for_background_save();
        EXPECT_NE(processor.execute({"LASTSAVE"}), integer(0));
    }
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------- glob

struct GlobCase {
    const char* pattern;
    const char* text;
    bool matches;
    friend void PrintTo(const GlobCase& c, std::ostream* os) {
        *os << "'" << c.pattern << "' vs '" << c.text << "'";
    }
};

class Glob : public ::testing::TestWithParam<GlobCase> {};

TEST_P(Glob, Matches) {
    const auto& [pattern, text, matches] = GetParam();
    EXPECT_EQ(glob_match(pattern, text), matches) << "pattern '" << pattern << "' vs '" << text << "'";
}

INSTANTIATE_TEST_SUITE_P(Patterns, Glob,
                         ::testing::Values(GlobCase{"*", "", true}, GlobCase{"*", "anything", true},
                                           GlobCase{"user:*", "user:42", true}, GlobCase{"user:*", "users", false},
                                           GlobCase{"h?llo", "hello", true}, GlobCase{"h?llo", "hllo", false},
                                           GlobCase{"h[ae]llo", "hallo", true}, GlobCase{"h[ae]llo", "hillo", false},
                                           GlobCase{"h[^e]llo", "hallo", true}, GlobCase{"h[^e]llo", "hello", false},
                                           GlobCase{"h[a-c]llo", "hbllo", true}, GlobCase{"h[a-c]llo", "hdllo", false},
                                           GlobCase{"a*b*c", "aXXbYYc", true}, GlobCase{"a*b*c", "aXXcYYb", false},
                                           GlobCase{"*.txt", "notes.txt.bak", false}, GlobCase{"\\*", "*", true},
                                           GlobCase{"\\*", "x", false}, GlobCase{"[", "[", true},
                                           GlobCase{"**a", "bbba", true}, GlobCase{"exact", "exact", true}),
                         [](const ::testing::TestParamInfo<GlobCase>& param) { return "case" + std::to_string(param.index); });
