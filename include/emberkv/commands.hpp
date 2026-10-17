#pragma once

#include "emberkv/resp.hpp"
#include "emberkv/snapshot.hpp"
#include "emberkv/store.hpp"
#include "emberkv/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace emberkv {

/// Connection counters owned by the server and reported by INFO.
struct ServerCounters {
    std::atomic<std::uint64_t> connected_clients{0};
    std::atomic<std::uint64_t> total_connections{0};
    std::atomic<std::uint64_t> total_commands{0};
    std::atomic<std::uint64_t> rejected_connections{0};
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
};

/// Turns a parsed request into a reply. Stateless apart from persistence
/// bookkeeping, so one instance is shared by every I/O thread.
class CommandProcessor {
public:
    struct Options {
        std::optional<std::filesystem::path> snapshot_path;
        ServerCounters* counters = nullptr;  // optional; enables the INFO client section
        std::size_t io_threads = 0;          // reported by INFO
    };

    struct CommandInfo {
        std::string_view name;
        int arity;  // Redis convention: N = exactly N args (name included), -N = at least N
        std::string_view summary;
    };

    explicit CommandProcessor(Store& store) : CommandProcessor(store, Options{}) {}
    CommandProcessor(Store& store, Options options);
    ~CommandProcessor();

    CommandProcessor(const CommandProcessor&) = delete;
    CommandProcessor& operator=(const CommandProcessor&) = delete;

    /// Executes one request; argv[0] is the case-insensitive command name.
    /// Takes argv by value so SET/LPUSH/... can move values into the store
    /// instead of copying them. Thread-safe.
    [[nodiscard]] resp::Reply execute(std::vector<std::string> argv);

    /// Blocks until an in-flight BGSAVE finishes (used at shutdown).
    void wait_for_background_save();

    [[nodiscard]] static std::span<const CommandInfo> commands() noexcept;

private:
    using Args = std::span<std::string>;
    using Handler = resp::Reply (CommandProcessor::*)(Args);
    struct Spec;
    static std::span<const Spec> table() noexcept;
    static const Spec* lookup(std::string_view name);

    // connection / server
    resp::Reply ping(Args);
    resp::Reply echo(Args);
    resp::Reply quit(Args);
    resp::Reply info(Args);
    resp::Reply dbsize(Args);
    resp::Reply flushall(Args);
    resp::Reply command(Args);
    resp::Reply config(Args);
    resp::Reply client(Args);
    resp::Reply help(Args);
    // strings & counters
    resp::Reply get(Args);
    resp::Reply set(Args);
    resp::Reply getdel(Args);
    resp::Reply append(Args);
    resp::Reply strlen(Args);
    resp::Reply incr(Args);
    resp::Reply decr(Args);
    resp::Reply incrby(Args);
    resp::Reply decrby(Args);
    resp::Reply mset(Args);
    resp::Reply mget(Args);
    // keyspace
    resp::Reply del(Args);
    resp::Reply exists(Args);
    resp::Reply type(Args);
    resp::Reply keys(Args);
    resp::Reply expire(Args);
    resp::Reply pexpire(Args);
    resp::Reply ttl(Args);
    resp::Reply pttl(Args);
    resp::Reply persist(Args);
    // lists
    resp::Reply lpush(Args);
    resp::Reply rpush(Args);
    resp::Reply lpop(Args);
    resp::Reply rpop(Args);
    resp::Reply llen(Args);
    resp::Reply lrange(Args);
    // persistence
    resp::Reply save(Args);
    resp::Reply bgsave(Args);
    resp::Reply lastsave(Args);

    resp::Reply incr_by(std::string_view key, std::int64_t delta);
    resp::Reply expire_in(std::string_view key, std::int64_t amount, std::chrono::milliseconds unit);
    resp::Reply ttl_in(std::string_view key, bool millis);
    resp::Reply push(Args args, bool front);
    resp::Reply pop(Args args, bool front);
    void record_save(const SnapshotInfo& info);

    Store& store_;
    Options options_;
    std::chrono::steady_clock::time_point started_at_ = std::chrono::steady_clock::now();

    std::mutex save_mutex_;  // serialises SAVE and BGSAVE (they share the .tmp file)
    std::future<void> bgsave_;            // guarded by save_mutex_
    std::atomic<bool> bgsave_running_{false};  // lock-free view for INFO
    std::atomic<std::int64_t> last_save_unix_{0};
    std::atomic<bool> last_save_ok_{true};
    std::atomic<std::size_t> last_save_keys_{0};

    ThreadPool saver_{1};  // last: its thread is joined before anything it uses dies
};

/// Redis-style glob: *, ?, [abc], [a-z], [^x] and \ escapes. Used by KEYS.
[[nodiscard]] bool glob_match(std::string_view pattern, std::string_view text) noexcept;

}  // namespace emberkv
