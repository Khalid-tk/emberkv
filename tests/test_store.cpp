#include "emberkv/commands.hpp"
#include "emberkv/store.hpp"
#include "emberkv/thread_pool.hpp"
#include "test_util.hpp"

#include <gtest/gtest.h>

#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace emberkv;
using namespace emberkv::testing;

TEST(Store, ShardCountIsRoundedUpToAPowerOfTwo) {
    EXPECT_EQ(Store(Store::Options{.shard_count = 10}).shard_count(), 16u);
    EXPECT_EQ(Store(Store::Options{.shard_count = 1}).shard_count(), 1u);
    EXPECT_EQ(Store(Store::Options{.shard_count = 0}).shard_count(), 1u);
    EXPECT_EQ(Store(Store::Options{.shard_count = 64}).shard_count(), 64u);
}

TEST(Store, KeysSpreadAcrossShards) {
    Store store(Store::Options{.shard_count = 16});
    CommandProcessor processor(store);
    for (int i = 0; i < 1600; ++i) (void)processor.execute({"SET", "key:" + std::to_string(i), "v"});

    std::vector<std::size_t> per_shard;
    store.for_each_shard([&](Store::Cache& cache, TimePoint) { per_shard.push_back(cache.size()); });
    for (std::size_t n : per_shard) {
        EXPECT_GT(n, 50u) << "badly skewed shard";
        EXPECT_LT(n, 150u) << "badly skewed shard";
    }
}

// maxmemory is one budget shared by all shards, not a fixed slice per shard:
// a single key may use far more than max_memory / shard_count.
TEST(Store, OneKeyMayUseMoreThanAShardsShare) {
    Store store(Store::Options{.shard_count = 64, .max_memory = 1024 * 1024});
    CommandProcessor processor(store);
    for (int i = 0; i < 20000; ++i) ASSERT_FALSE(is_error(processor.execute({"RPUSH", "big", "item"})));
    EXPECT_EQ(processor.execute({"LLEN", "big"}), integer(20000));
    EXPECT_GT(store.stats().used_memory, 1024u * 1024 / 64);
}

TEST(Store, MemoryLimitIsEnforcedAcrossShards) {
    constexpr std::size_t kLimit = 256 * 1024;
    Store store(Store::Options{.shard_count = 8, .max_memory = kLimit});
    CommandProcessor processor(store);
    for (int i = 0; i < 5000; ++i) (void)processor.execute({"SET", "key:" + std::to_string(i), std::string(100, 'v')});

    // Writes evict as they go; at most a single entry of slack can remain...
    EXPECT_LE(store.stats().used_memory, kLimit + 512);
    // ...which the background pass removes.
    store.enforce_memory_limit();
    EXPECT_LE(store.stats().used_memory, kLimit);
    EXPECT_GT(store.stats().evictions, 0u);
}

TEST(Store, BackgroundSweeperRemovesExpiredKeys) {
    ManualClock clock;
    Store store(Store::Options{.shard_count = 4, .clock = clock.function()});
    CommandProcessor processor(store);
    for (int i = 0; i < 100; ++i) (void)processor.execute({"SET", "k" + std::to_string(i), "v", "PX", "50"});
    (void)processor.execute({"SET", "keeper", "v"});
    EXPECT_EQ(store.stats().keys, 101u);

    clock.advance(50ms);
    store.start_expiry_sweeper(5ms);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (store.stats().keys > 1 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);

    EXPECT_EQ(store.stats().keys, 1u) << "expired keys are removed without being touched";
    EXPECT_EQ(store.stats().expirations, 100u);
}

// Many threads incrementing one key: INCR's read-modify-write must be atomic.
TEST(StoreConcurrency, IncrementsAreAtomic) {
    Store store;
    CommandProcessor processor(store);
    ThreadPool pool(8);
    constexpr int kTasks = 8, kPerTask = 5000;

    std::vector<std::future<void>> done;
    for (int t = 0; t < kTasks; ++t)
        done.push_back(pool.submit([&] {
            for (int i = 0; i < kPerTask; ++i) (void)processor.execute({"INCR", "counter"});
        }));
    for (auto& f : done) f.get();

    EXPECT_EQ(processor.execute({"GET", "counter"}), bulk(std::to_string(kTasks * kPerTask)));
}

// Multi-key commands lock several shards. Threads asking for the same keys in
// opposite orders would deadlock without the ordered locking in with_keys().
TEST(StoreConcurrency, MultiKeyCommandsDoNotDeadlock) {
    Store store(Store::Options{.shard_count = 8});
    CommandProcessor processor(store);
    ThreadPool pool(6);

    std::vector<std::future<void>> done;
    for (int t = 0; t < 6; ++t) {
        done.push_back(pool.submit(
            [&](unsigned seed) {
                std::mt19937 rng(seed);
                std::uniform_int_distribution<int> key(0, 31);
                for (int i = 0; i < 3000; ++i) {
                    auto k = [&] { return "k" + std::to_string(key(rng)); };
                    switch (i % 3) {
                        case 0: (void)processor.execute({"MSET", k(), "1", k(), "2", k(), "3"}); break;
                        case 1: (void)processor.execute({"DEL", k(), k(), k()}); break;
                        default: (void)processor.execute({"MGET", k(), k(), k(), k()}); break;
                    }
                }
            },
            static_cast<unsigned>(t)));
    }
    for (auto& f : done) ASSERT_EQ(f.wait_for(30s), std::future_status::ready) << "deadlock";
}

TEST(StoreConcurrency, MixedWorkloadKeepsAccountingConsistent) {
    Store store(Store::Options{.shard_count = 4, .max_memory = 256 * 1024});
    CommandProcessor processor(store);
    ThreadPool pool(4);
    std::vector<std::future<void>> done;
    for (int t = 0; t < 4; ++t) {
        done.push_back(pool.submit([&, t] {
            for (int i = 0; i < 4000; ++i) {
                const std::string key = "k" + std::to_string((i * 7 + t) % 500);
                (void)processor.execute({"SET", key, std::string(static_cast<std::size_t>(i % 300), 'x')});
                (void)processor.execute({"RPUSH", "list" + std::to_string(t), "item"});
                (void)processor.execute({"APPEND", key, "tail"});
                (void)processor.execute({"GET", key});
            }
        }));
    }
    for (auto& f : done) f.get();

    // Recompute the weight from scratch and compare with the running total.
    store.for_each_shard([](Store::Cache& cache, TimePoint now) {
        std::size_t total = 0;
        cache.for_each(now, [&](const Store::Cache::Entry& e) { total += EntryWeigher{}(e.key(), e.value()); });
        EXPECT_EQ(total, cache.weight());
        EXPECT_LE(cache.weight(), cache.capacity());
    });
}

// ----------------------------------------------------------------- thread pool

TEST(ThreadPool, ReturnsResultsThroughFutures) {
    ThreadPool pool(3);
    std::vector<std::future<int>> results;
    for (int i = 0; i < 100; ++i) results.push_back(pool.submit([](int x) { return x * x; }, i));
    for (int i = 0; i < 100; ++i) EXPECT_EQ(results[static_cast<std::size_t>(i)].get(), i * i);
}

TEST(ThreadPool, PropagatesExceptions) {
    ThreadPool pool(1);
    auto f = pool.submit([]() -> int { throw std::runtime_error("task failed"); });
    EXPECT_THROW((void)f.get(), std::runtime_error);
}

TEST(ThreadPool, AcceptsMoveOnlyArguments) {
    ThreadPool pool(1);
    auto f = pool.submit([](std::unique_ptr<int> p) { return *p + 1; }, std::make_unique<int>(41));
    EXPECT_EQ(f.get(), 42);
}

TEST(ThreadPool, DestructorFinishesQueuedWork) {
    std::atomic<int> ran{0};
    {
        ThreadPool pool(2);
        for (int i = 0; i < 50; ++i)
            (void)pool.submit([&] {
                std::this_thread::sleep_for(1ms);
                ran.fetch_add(1);
            });
    }
    EXPECT_EQ(ran.load(), 50);
}
