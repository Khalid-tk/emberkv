#pragma once

// The thread-safe keyspace. Keys are spread over N independent shards, each
// an LruCache behind its own mutex, so threads touching different keys rarely
// contend. Why std::mutex and not std::shared_mutex: in an LRU, a read *is* a
// write (it moves the entry to the front), so a reader/writer lock would buy
// nothing and cost more.

#include "emberkv/lru_cache.hpp"
#include "emberkv/value.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

namespace emberkv {

struct StoreStats {
    std::size_t keys = 0;
    std::size_t keys_with_expiry = 0;
    std::size_t used_memory = 0;
    std::size_t max_memory = 0;
    std::uint64_t evictions = 0;
    std::uint64_t expirations = 0;
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
};

class Store {
public:
    using Cache = LruCache<std::string, Value, EntryWeigher>;
    using Clock = std::function<TimePoint()>;

    struct Options {
        std::size_t shard_count = 16;               // rounded up to a power of two
        std::size_t max_memory = 256u * 1024 * 1024;  // bytes, one budget shared by all shards
        Clock clock = [] { return std::chrono::steady_clock::now(); };
    };

    Store() : Store(Options{}) {}
    explicit Store(Options options);
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    /// Runs f(cache, now) with the shard that owns `key` locked. Every
    /// single-key command goes through here, which is what makes
    /// read-modify-write commands such as INCR atomic.
    /// `f` must not call back into the Store (the mutex is not recursive).
    template <class F>
        requires std::invocable<F, Cache&, TimePoint>
    decltype(auto) with_key(std::string_view key, F&& f) {
        Shard& shard = *shards_[shard_index(key)];
        std::scoped_lock lock(shard.mutex);
        return std::invoke(std::forward<F>(f), shard.cache, now());
    }

    /// Multi-key variant (MSET, MGET, DEL k1 k2 ...). Locks every shard the
    /// keys map to, always in ascending shard order, so two threads locking
    /// overlapping sets can never deadlock. f receives the cache for each key
    /// (caches[i] owns keys[i]), so no key is hashed to a shard twice.
    template <class F>
        requires std::invocable<F, std::span<Cache* const>, TimePoint>
    decltype(auto) with_keys(std::span<const std::string_view> keys, F&& f) {
        std::vector<std::size_t> shard_of(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) shard_of[i] = shard_index(keys[i]);

        std::vector<std::size_t> lock_order = shard_of;
        std::ranges::sort(lock_order);
        const auto [first, last] = std::ranges::unique(lock_order);
        lock_order.erase(first, last);

        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(lock_order.size());
        for (std::size_t id : lock_order) locks.emplace_back(shards_[id]->mutex);

        std::vector<Cache*> caches(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) caches[i] = &shards_[shard_of[i]]->cache;
        return std::invoke(std::forward<F>(f), std::span<Cache* const>(caches), now());
    }

    /// Visits each shard in turn, one lock at a time. The result is a
    /// per-shard consistent view, not a global point-in-time snapshot.
    template <class F>
        requires std::invocable<F&, Cache&, TimePoint>
    void for_each_shard(F&& f) {
        for (auto& shard : shards_) {
            std::scoped_lock lock(shard->mutex);
            f(shard->cache, now());
        }
    }

    /// Removes keys whose TTL has passed. Called by the background sweeper;
    /// `budget_per_shard` bounds how long any one lock is held.
    std::size_t evict_expired(std::size_t budget_per_shard = 256);

    /// If total memory is over budget, evicts LRU entries shard by shard until
    /// it isn't. Writes already evict as they go; this catches the case where
    /// the over-budget shards are not the ones being written to.
    std::size_t enforce_memory_limit();

    /// Starts a background thread that, every `interval`, removes expired keys
    /// and enforces the memory limit.
    /// Stopped automatically when the Store is destroyed.
    void start_expiry_sweeper(std::chrono::milliseconds interval = std::chrono::milliseconds{100});

    void clear();

    void record_lookup(bool hit) noexcept {
        (hit ? hits_ : misses_).fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] StoreStats stats() const;
    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }
    [[nodiscard]] TimePoint now() const { return clock_(); }

private:
    // Each shard is aligned to its own cache line so two threads hammering
    // neighbouring mutexes don't false-share a line.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)  // "structure was padded due to alignment": that is the intent
#endif
    struct alignas(64) Shard {
        Shard(std::size_t capacity, MemoryBudget& budget) : cache(capacity, EntryWeigher{}, &budget) {}
        mutable std::mutex mutex;
        Cache cache;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    /// Fibonacci hashing: multiply by 2^64/phi and keep the top bits. Using the
    /// *high* bits matters: each shard's unordered_map buckets by the *low*
    /// bits of the same hash (MSVC's uses a power-of-two mask), so picking the
    /// shard from the low bits would make every key in a shard collide.
    [[nodiscard]] std::size_t shard_index(std::string_view key) const noexcept {
        if (shard_bits_ == 0) return 0;
        const auto h = static_cast<std::uint64_t>(StringHash{}(key));
        return static_cast<std::size_t>((h * 0x9E3779B97F4A7C15ull) >> (64 - shard_bits_));
    }

    Clock clock_;
    MemoryBudget budget_;  // declared before shards_: they report to it until destroyed
    std::vector<std::unique_ptr<Shard>> shards_;
    unsigned shard_bits_ = 0;  // log2(shards_.size())
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};

    // Declared last so it is destroyed (stopped and joined) first, while the
    // shards it touches are still alive.
    std::condition_variable_any sweeper_cv_;
    std::mutex sweeper_mutex_;
    std::jthread sweeper_;
};

}  // namespace emberkv
