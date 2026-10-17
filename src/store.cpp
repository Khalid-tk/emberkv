#include "emberkv/store.hpp"

#include <bit>
#include <stdexcept>

namespace emberkv {

Store::Store(Options options) : clock_(std::move(options.clock)), budget_(options.max_memory) {
    if (!clock_) throw std::invalid_argument("Store: clock must be callable");
    const std::size_t count = std::bit_ceil(std::max<std::size_t>(options.shard_count, 1));
    shard_bits_ = static_cast<unsigned>(std::countr_zero(count));
    shards_.reserve(count);
    // Every shard may grow to the full budget; the shared MemoryBudget is
    // what keeps the *sum* in check. A fixed per-shard slice would cap any
    // single key (say, one large list) at max_memory / shard_count.
    for (std::size_t i = 0; i < count; ++i) shards_.push_back(std::make_unique<Shard>(options.max_memory, budget_));
}

Store::~Store() {
    if (sweeper_.joinable()) {
        sweeper_.request_stop();  // wakes the condition variable wait below
        sweeper_.join();
    }
}

std::size_t Store::evict_expired(std::size_t budget_per_shard) {
    std::size_t removed = 0;
    for_each_shard([&](Cache& cache, TimePoint now) { removed += cache.evict_expired(now, budget_per_shard); });
    return removed;
}

std::size_t Store::enforce_memory_limit() {
    std::size_t evicted = 0;
    for (auto& shard : shards_) {
        if (!budget_.exceeded()) break;
        std::scoped_lock lock(shard->mutex);
        evicted += shard->cache.trim();
    }
    return evicted;
}

void Store::start_expiry_sweeper(std::chrono::milliseconds interval) {
    if (sweeper_.joinable()) return;
    sweeper_ = std::jthread([this, interval](std::stop_token stop) {
        std::unique_lock lock(sweeper_mutex_);
        // The stop_token overload of wait_for wakes up as soon as stop is
        // requested, so shutdown never waits out a full interval. The
        // predicate is always false: we only ever want the timeout or the stop.
        while (!sweeper_cv_.wait_for(lock, stop, interval, [] { return false; })) {
            if (stop.stop_requested()) break;
            lock.unlock();
            evict_expired();
            enforce_memory_limit();
            lock.lock();
        }
    });
}

void Store::clear() {
    for_each_shard([](Cache& cache, TimePoint) { cache.clear(); });
}

StoreStats Store::stats() const {
    StoreStats s;
    s.max_memory = budget_.limit;
    for (const auto& shard : shards_) {
        std::scoped_lock lock(shard->mutex);
        s.keys += shard->cache.size();
        s.keys_with_expiry += shard->cache.keys_with_expiry();
        s.used_memory += shard->cache.weight();
        s.evictions += shard->cache.stats().evictions;
        s.expirations += shard->cache.stats().expirations;
    }
    s.hits = hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace emberkv
