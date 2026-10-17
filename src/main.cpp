#include "emberkv/commands.hpp"
#include "emberkv/config.hpp"
#include "emberkv/net.hpp"
#include "emberkv/server.hpp"
#include "emberkv/snapshot.hpp"
#include "emberkv/store.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// A signal handler may only touch lock-free atomics (or volatile sig_atomic_t).
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "the signal handler needs a lock-free flag");

extern "C" void on_signal(int) { g_stop.store(true); }

double megabytes(std::size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

}  // namespace

int main(int argc, char** argv) {
    using namespace emberkv;
    using namespace std::chrono_literals;

    const std::vector<std::string_view> args(argv + 1, argv + argc);
    Config config;
    try {
        config = parse_args(args);
    } catch (const std::invalid_argument& e) {
        std::fprintf(stderr, "emberkv: %s\n", e.what());
        return 2;
    }
    if (config.help) {
        std::fputs(usage("emberkv-server").c_str(), stdout);
        return 0;
    }
    if (config.version) {
        std::puts("emberkv 1.0.0");
        return 0;
    }
    if (config.server.io_threads == 0)
        config.server.io_threads = std::max(1u, std::thread::hardware_concurrency());

    try {
        net::NetworkInit network;

        Store store(Store::Options{.shard_count = config.shards, .max_memory = config.max_memory});
        store.start_expiry_sweeper();

        if (config.snapshot && std::filesystem::exists(*config.snapshot)) {
            const SnapshotInfo info = load_snapshot(store, *config.snapshot);
            std::printf("Loaded %zu keys from %s\n", info.keys, config.snapshot->string().c_str());
        }

        ServerCounters counters;
        CommandProcessor processor(store, {.snapshot_path = config.snapshot,
                                           .counters = &counters,
                                           .io_threads = config.server.io_threads});
        Server server(config.server, processor, counters);
        server.start();

        std::printf("EmberKV 1.0.0 listening on %s:%u\n", config.server.bind.c_str(), server.port());
        std::printf("  %zu I/O threads, %zu shards, %s poller, maxmemory %.0f MB\n", server.io_threads(),
                    store.shard_count(), std::string(net::Poller::backend()).c_str(), megabytes(config.max_memory));
        std::printf("  Press Ctrl+C to stop.\n");
        std::fflush(stdout);

        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
        // Polling a flag is the portable way out of a signal handler: almost
        // nothing (including condition variables) is async-signal-safe.
        while (!g_stop.load()) std::this_thread::sleep_for(100ms);

        std::printf("\nShutting down...\n");
        server.stop();
        processor.wait_for_background_save();
        if (config.snapshot) {
            const SnapshotInfo info = save_snapshot(store, *config.snapshot);
            std::printf("Saved %zu keys to %s\n", info.keys, config.snapshot->string().c_str());
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "emberkv: fatal: %s\n", e.what());
        return 1;
    }
    return 0;
}
