#pragma once

// A generic, single-threaded LRU cache with per-entry TTL and weight-based
// capacity. Thread safety is layered on top by emberkv::Store, which owns one
// LruCache per shard and guards each with its own mutex.
//
// Data structures (all operations O(1) average, except TTL index ops which
// are O(log n) in the number of keys that carry an expiry):
//
//   order_   std::list<Entry>                  recency order, MRU at front
//   index_   std::unordered_map<KeyRef, list::it>  key -> node in order_
//            (KeyRef points at the key inside the node: each key is stored once)
//   expiry_  std::multimap<time, list::it>     soonest deadline first
//
// std::list is chosen deliberately: splice() moves a node to the front without
// invalidating any iterator, so the iterators stored in index_ and expiry_
// stay valid for the lifetime of the entry.

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

// MSVC accepts [[no_unique_address]] but ignores it; it has its own spelling.
#if defined(_MSC_VER) && !defined(__clang__)
#define EMBERKV_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define EMBERKV_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

namespace emberkv {

using TimePoint = std::chrono::steady_clock::time_point;

/// A weigher maps an entry to its cost against the cache capacity.
template <class W, class K, class V>
concept Weigher = std::regular_invocable<const W&, const K&, const V&> &&
                  std::convertible_to<std::invoke_result_t<const W&, const K&, const V&>, std::size_t>;

/// Every entry costs 1: capacity becomes a maximum entry count.
struct UnitWeigher {
    template <class K, class V>
    constexpr std::size_t operator()(const K&, const V&) const noexcept {
        return 1;
    }
};

/// Transparent hash: lets unordered_map<std::string, ...>::find accept a
/// std::string_view without first allocating a temporary std::string.
struct StringHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

namespace detail {
template <class K>
struct HashTraits {
    using hash = std::hash<K>;
    using equal = std::equal_to<K>;
};
template <>
struct HashTraits<std::string> {
    using hash = StringHash;
    using equal = std::equal_to<>;  // transparent
};
}  // namespace detail

/// A memory limit shared by several caches (the store's shards). Each cache
/// adds and removes its own weight here; when the total is over the limit, a
/// cache that is being written to evicts its own least-recently-used entries.
/// That gives one global maxmemory without any cache locking another.
struct MemoryBudget {
    explicit MemoryBudget(std::size_t limit_bytes) noexcept : limit(limit_bytes) {}
    const std::size_t limit;
    std::atomic<std::size_t> used{0};
    [[nodiscard]] bool exceeded() const noexcept { return used.load(std::memory_order_relaxed) > limit; }
};

struct CacheStats {
    std::uint64_t evictions = 0;    // removed to make room
    std::uint64_t expirations = 0;  // removed because their TTL passed
};

template <class K, class V, class W = UnitWeigher, class Hash = typename detail::HashTraits<K>::hash,
          class KeyEqual = typename detail::HashTraits<K>::equal>
    requires Weigher<W, K, V>
class LruCache {
public:
    class Entry;

private:
    using List = std::list<Entry>;
    using ListIt = typename List::iterator;
    using ExpiryIndex = std::multimap<TimePoint, ListIt>;

    // The index refers to the key stored inside the list node instead of
    // holding a second copy: one allocation and one key's worth of memory
    // saved per entry. Safe because list nodes never move.
    struct KeyRef {
        const K* key;
    };
    template <class Q>
    static constexpr bool is_key_ref = std::same_as<std::remove_cvref_t<Q>, KeyRef>;

    struct RefHash {
        using is_transparent = void;
        EMBERKV_NO_UNIQUE_ADDRESS Hash hash;
        std::size_t operator()(const KeyRef& r) const { return hash(*r.key); }
        template <class Q>
            requires(!is_key_ref<Q>)
        std::size_t operator()(const Q& q) const {
            return hash(q);
        }
    };
    struct RefEqual {
        using is_transparent = void;
        EMBERKV_NO_UNIQUE_ADDRESS KeyEqual eq;
        bool operator()(const KeyRef& a, const KeyRef& b) const { return eq(*a.key, *b.key); }
        template <class Q>
            requires(!is_key_ref<Q>)
        bool operator()(const KeyRef& a, const Q& b) const {
            return eq(*a.key, b);
        }
        template <class Q>
            requires(!is_key_ref<Q>)
        bool operator()(const Q& a, const KeyRef& b) const {
            return eq(a, *b.key);
        }
    };
    using Index = std::unordered_map<KeyRef, ListIt, RefHash, RefEqual>;

public:
    using key_type = K;
    using mapped_type = V;

    /// What callers see. Key and bookkeeping are read-only; the value is
    /// mutable in place, after which the caller must call reweigh().
    class Entry {
    public:
        Entry(K key, V value, std::size_t weight) : key_(std::move(key)), value_(std::move(value)), weight_(weight) {}

        [[nodiscard]] const K& key() const noexcept { return key_; }
        [[nodiscard]] V& value() noexcept { return value_; }
        [[nodiscard]] const V& value() const noexcept { return value_; }
        [[nodiscard]] std::size_t weight() const noexcept { return weight_; }

        [[nodiscard]] std::optional<TimePoint> expires_at() const noexcept {
            if (expiry_) return (*expiry_)->first;
            return std::nullopt;
        }
        [[nodiscard]] bool expired(TimePoint now) const noexcept { return expiry_ && (*expiry_)->first <= now; }

    private:
        friend class LruCache;
        K key_;
        V value_;
        std::size_t weight_;
        std::optional<typename ExpiryIndex::iterator> expiry_;
    };

    enum class PutResult { inserted, replaced, too_large };

    /// `capacity` bounds this cache's own weight (and the largest single entry).
    /// `budget`, if given, is an additional limit shared with other caches.
    explicit LruCache(std::size_t capacity, W weigher = W{}, MemoryBudget* budget = nullptr)
        : capacity_(capacity), weigher_(std::move(weigher)), budget_(budget) {}

    // Copying would leave the copied index pointing into the *source* list, so
    // copies are disabled. Moves are fine: moving node-based containers keeps
    // every iterator valid, now referring into the destination. They are
    // written out (not defaulted) because the source must hand over its share
    // of the budget, or it would be released twice.
    LruCache(const LruCache&) = delete;
    LruCache& operator=(const LruCache&) = delete;

    LruCache(LruCache&& other) noexcept
        : capacity_(other.capacity_),
          total_weight_(std::exchange(other.total_weight_, 0)),
          weigher_(std::move(other.weigher_)),
          budget_(std::exchange(other.budget_, nullptr)),
          order_(std::move(other.order_)),
          index_(std::move(other.index_)),
          expiry_(std::move(other.expiry_)),
          stats_(other.stats_) {
        other.clear();
    }

    LruCache& operator=(LruCache&& other) noexcept {
        if (this != &other) {
            clear();  // releases our budget share
            capacity_ = other.capacity_;
            total_weight_ = std::exchange(other.total_weight_, 0);
            weigher_ = std::move(other.weigher_);
            budget_ = std::exchange(other.budget_, nullptr);
            order_ = std::move(other.order_);
            index_ = std::move(other.index_);
            expiry_ = std::move(other.expiry_);
            stats_ = other.stats_;
            other.clear();
        }
        return *this;
    }

    ~LruCache() { release(total_weight_); }

    /// Looks up a live entry and marks it most-recently-used.
    /// An expired entry is removed on the spot (lazy expiration).
    template <class Q>
    [[nodiscard]] Entry* find(const Q& key, TimePoint now) {
        auto it = index_.find(key);
        if (it == index_.end()) return nullptr;
        if (it->second->expired(now)) {
            erase_node(it);
            ++stats_.expirations;
            return nullptr;
        }
        order_.splice(order_.begin(), order_, it->second);
        return &*it->second;
    }

    /// Looks up a live entry without touching recency order.
    template <class Q>
    [[nodiscard]] const Entry* peek(const Q& key, TimePoint now) const {
        auto it = index_.find(key);
        if (it == index_.end() || it->second->expired(now)) return nullptr;
        return &*it->second;
    }

    /// Inserts or replaces. Strong exception guarantee: if allocation fails,
    /// the cache is left exactly as it was. The key is forwarded, so replacing
    /// an existing entry never copies it, and inserting copies it at most once.
    template <class Q>
        requires std::constructible_from<K, Q&&>
    PutResult put(Q&& key, V value, std::optional<TimePoint> expires_at = std::nullopt) {
        const auto w = static_cast<std::size_t>(std::invoke(weigher_, std::as_const(key), std::as_const(value)));
        if (w > capacity_) return PutResult::too_large;

        if (auto it = index_.find(key); it != index_.end()) {
            Entry& e = *it->second;
            if (expires_at) {
                // Allocate the new expiry node first: if it throws, nothing changed.
                auto exp_it = expiry_.emplace(*expires_at, it->second);
                if (e.expiry_) expiry_.erase(*e.expiry_);
                e.expiry_ = exp_it;
            } else if (e.expiry_) {
                expiry_.erase(*e.expiry_);
                e.expiry_.reset();
            }
            adjust(e.weight_, w);
            e.value_ = std::move(value);
            e.weight_ = w;
            order_.splice(order_.begin(), order_, it->second);
            evict_until_fits();
            return PutResult::replaced;
        }

        // Three containers must change together. Each step that can throw is
        // undone by iterator (never by key lookup, which could throw again).
        order_.emplace_front(K(std::forward<Q>(key)), std::move(value), w);
        typename Index::iterator slot;
        try {
            slot = index_.emplace(KeyRef{&order_.front().key_}, order_.begin()).first;
        } catch (...) {
            order_.pop_front();
            throw;
        }
        if (expires_at) {
            try {
                order_.front().expiry_ = expiry_.emplace(*expires_at, order_.begin());
            } catch (...) {
                index_.erase(slot);
                order_.pop_front();
                throw;
            }
        }
        charge(w);
        evict_until_fits();
        return PutResult::inserted;
    }

    /// Sets or clears the deadline of an entry previously returned by find().
    void set_expiry(Entry& e, std::optional<TimePoint> expires_at) {
        if (e.expiry_) {
            expiry_.erase(*e.expiry_);
            e.expiry_.reset();
        }
        if (expires_at) e.expiry_ = expiry_.emplace(*expires_at, index_.find(e.key_)->second);
    }

    /// Call after mutating Entry::value() in place. Re-computes the weight and
    /// evicts *other* entries if needed. Returns false if the entry alone no
    /// longer fits, in which case it has been removed.
    bool reweigh(Entry& e) {
        const std::size_t w = weigh(e.key_, e.value_);
        auto it = index_.find(e.key_);
        adjust(e.weight_, w);
        e.weight_ = w;
        if (w > capacity_) {
            erase_node(it);
            return false;
        }
        order_.splice(order_.begin(), order_, it->second);
        evict_until_fits();
        return true;
    }

    template <class Q>
    bool erase(const Q& key) {
        auto it = index_.find(key);
        if (it == index_.end()) return false;
        erase_node(it);
        return true;
    }

    /// Active expiration: removes up to `limit` entries whose deadline passed.
    /// Cheap because expiry_ is ordered by deadline: we only touch dead keys.
    std::size_t evict_expired(TimePoint now, std::size_t limit = SIZE_MAX) {
        std::size_t removed = 0;
        while (removed < limit && !expiry_.empty() && expiry_.begin()->first <= now) {
            erase_node(index_.find(expiry_.begin()->second->key_));
            ++removed;
        }
        stats_.expirations += removed;
        return removed;
    }

    /// Visits live entries from most- to least-recently used.
    template <class F>
        requires std::invocable<F&, const Entry&>
    void for_each(TimePoint now, F&& visit) const {
        for (const Entry& e : order_)
            if (!e.expired(now)) visit(e);
    }

    void set_capacity(std::size_t capacity) {
        capacity_ = capacity;
        evict_until_fits();
    }

    /// Evicts least-recently-used entries until this cache, and the shared
    /// budget, are within limits (or the cache is empty). The store's
    /// background thread calls this so the budget converges even when the
    /// shards that are over it see no writes.
    std::size_t trim() noexcept {
        std::size_t evicted = 0;
        while (!order_.empty() && (total_weight_ > capacity_ || (budget_ && budget_->exceeded()))) {
            erase_node(index_.find(std::prev(order_.end())->key_));
            ++evicted;
        }
        stats_.evictions += evicted;
        return evicted;
    }

    void clear() noexcept {
        release(total_weight_);
        expiry_.clear();
        index_.clear();
        order_.clear();
    }

    [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
    [[nodiscard]] bool empty() const noexcept { return index_.empty(); }
    [[nodiscard]] std::size_t weight() const noexcept { return total_weight_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t keys_with_expiry() const noexcept { return expiry_.size(); }
    [[nodiscard]] const CacheStats& stats() const noexcept { return stats_; }

private:
    std::size_t weigh(const K& key, const V& value) const {
        return static_cast<std::size_t>(std::invoke(weigher_, key, value));
    }

    void erase_node(typename Index::iterator it) noexcept {
        ListIt node = it->second;
        release(node->weight_);
        if (node->expiry_) expiry_.erase(*node->expiry_);
        index_.erase(it);
        order_.erase(node);
    }

    void charge(std::size_t w) noexcept {
        total_weight_ += w;
        if (budget_ && w != 0) budget_->used.fetch_add(w, std::memory_order_relaxed);
    }
    void release(std::size_t w) noexcept {
        total_weight_ -= w;
        if (budget_ && w != 0) budget_->used.fetch_sub(w, std::memory_order_relaxed);
    }
    /// One atomic read-modify-write (or none) instead of a release + charge:
    /// the budget counter is shared by every shard, so its cache line is
    /// contended and each RMW on it is comparatively expensive.
    void adjust(std::size_t old_weight, std::size_t new_weight) noexcept {
        if (new_weight >= old_weight) charge(new_weight - old_weight);
        else release(old_weight - new_weight);
    }

    /// Runs after every write. The entry just written sits at the front and is
    /// never evicted to make room for itself: our own capacity can always be
    /// met without it (it was checked to fit), and for the shared budget we
    /// stop at one entry and let other shards' writes, or trim(), do the rest.
    void evict_until_fits() noexcept {
        while (!order_.empty() &&
               (total_weight_ > capacity_ || (budget_ && budget_->exceeded() && order_.size() > 1))) {
            erase_node(index_.find(std::prev(order_.end())->key_));
            ++stats_.evictions;
        }
    }

    std::size_t capacity_;
    std::size_t total_weight_ = 0;
    EMBERKV_NO_UNIQUE_ADDRESS W weigher_;  // stateless weighers take no space
    MemoryBudget* budget_ = nullptr;
    List order_;
    Index index_;
    ExpiryIndex expiry_;
    CacheStats stats_;
};

}  // namespace emberkv
