// Measures how lock sharding affects throughput under contention.
//
// Runs a 90% GET / 10% SET workload over 100k keys through the full command
// path (dispatch + store) for each combination of thread count and shard
// count, and prints a Markdown table. Network I/O is deliberately excluded:
// this isolates the cost of the data structure and its locking.
//
//   emberkv-bench [seconds-per-run]

#include "emberkv/commands.hpp"
#include "emberkv/store.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace emberkv;

namespace {

constexpr int kKeys = 100'000;

double run(std::size_t shards, unsigned threads, std::chrono::milliseconds duration) {
    Store store(Store::Options{.shard_count = shards, .max_memory = std::size_t{1} << 30});
    CommandProcessor processor(store);
    for (int i = 0; i < kKeys; ++i) (void)processor.execute({"SET", "key:" + std::to_string(i), "value"});

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> total{0};
    std::latch ready(static_cast<std::ptrdiff_t>(threads) + 1);  // C++20: start every thread at once

    std::vector<std::jthread> workers;
    for (unsigned t = 0; t < threads; ++t) {
        workers.emplace_back([&, t] {
            std::mt19937 rng(t + 1);
            std::uniform_int_distribution<int> key(0, kKeys - 1);
            std::uniform_int_distribution<int> percent(0, 99);
            std::uint64_t ops = 0;
            ready.arrive_and_wait();
            while (!stop.load(std::memory_order_relaxed)) {
                std::string k = "key:" + std::to_string(key(rng));
                if (percent(rng) < 90) (void)processor.execute({"GET", std::move(k)});
                else (void)processor.execute({"SET", std::move(k), "updated"});
                ++ops;
            }
            total.fetch_add(ops);
        });
    }

    ready.arrive_and_wait();
    const auto start = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(duration);
    stop.store(true);
    workers.clear();  // join
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<double>(total.load()) / elapsed.count();
}

}  // namespace

int main(int argc, char** argv) {
    const double seconds = argc > 1 ? std::atof(argv[1]) : 1.0;
    const auto duration = std::chrono::milliseconds(static_cast<long long>(seconds * 1000));
    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());

    std::vector<unsigned> thread_counts{1, 2, 4, 8};
    std::printf("Hardware threads: %u. %0.1fs per run, 90%% GET / 10%% SET, %d keys.\n\n", hw, seconds, kKeys);
    std::printf("| Threads | 1 shard (ops/s) | 64 shards (ops/s) | Speed-up |\n");
    std::printf("|--------:|----------------:|------------------:|---------:|\n");
    for (unsigned threads : thread_counts) {
        const double single = run(1, threads, duration);
        const double sharded = run(64, threads, duration);
        std::printf("| %7u | %15.0f | %17.0f | %7.2fx |\n", threads, single, sharded, sharded / single);
        std::fflush(stdout);
    }
    return 0;
}
