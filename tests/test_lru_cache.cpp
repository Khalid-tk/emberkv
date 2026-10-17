#include "emberkv/lru_cache.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <string_view>

using namespace emberkv;
using namespace std::chrono_literals;

namespace {

const TimePoint t0 = TimePoint{} + 1h;

struct SizeWeigher {
    std::size_t operator()(const std::string& k, const std::string& v) const noexcept { return k.size() + v.size(); }
};

using StringCache = LruCache<std::string, std::string>;

std::string value_of(StringCache& c, std::string_view key, TimePoint now = t0) {
    auto* e = c.find(key, now);
    return e ? e->value() : "<missing>";
}

}  // namespace

// Compile-time checks on the Weigher concept.
static_assert(Weigher<UnitWeigher, std::string, int>);
static_assert(Weigher<SizeWeigher, std::string, std::string>);
static_assert(!Weigher<SizeWeigher, std::string, int>, "a weigher must accept the value type");

TEST(LruCache, PutAndFind) {
    StringCache cache(10);
    EXPECT_EQ(cache.put("a", "1"), StringCache::PutResult::inserted);
    EXPECT_EQ(cache.put("a", "2"), StringCache::PutResult::replaced);
    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(value_of(cache, "a"), "2");
    EXPECT_EQ(cache.find("missing", t0), nullptr);
}

TEST(LruCache, LookupByStringViewDoesNotNeedAString) {
    StringCache cache(10);
    cache.put("key", "value");
    constexpr std::string_view sv = "key";  // heterogeneous lookup via the transparent hash
    ASSERT_NE(cache.find(sv, t0), nullptr);
    EXPECT_NE(cache.peek(sv, t0), nullptr);
    EXPECT_TRUE(cache.erase(sv));
}

TEST(LruCache, EvictsLeastRecentlyUsed) {
    StringCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");
    ASSERT_NE(cache.find("a", t0), nullptr);  // a becomes most recent; b is now oldest
    cache.put("d", "4");

    EXPECT_EQ(cache.size(), 3u);
    EXPECT_EQ(cache.find("b", t0), nullptr);
    EXPECT_EQ(value_of(cache, "a"), "1");
    EXPECT_EQ(cache.stats().evictions, 1u);
}

TEST(LruCache, PeekDoesNotChangeRecency) {
    StringCache cache(2);
    cache.put("a", "1");
    cache.put("b", "2");
    ASSERT_NE(cache.peek("a", t0), nullptr);  // does not save "a"
    cache.put("c", "3");
    EXPECT_EQ(cache.find("a", t0), nullptr);
}

TEST(LruCache, CapacityIsMeasuredInWeight) {
    LruCache<std::string, std::string, SizeWeigher> cache(10);
    cache.put("a", "1234");  // weight 5
    cache.put("b", "1234");  // weight 5, total 10
    EXPECT_EQ(cache.weight(), 10u);
    cache.put("c", "12");  // weight 3: "a" must go
    EXPECT_EQ(cache.find("a", t0), nullptr);
    EXPECT_EQ(cache.weight(), 8u);
}

TEST(LruCache, RejectsAnEntryLargerThanCapacityAndKeepsTheOldValue) {
    LruCache<std::string, std::string, SizeWeigher> cache(10);
    cache.put("k", "old");
    EXPECT_EQ(cache.put("k", std::string(100, 'x')), decltype(cache)::PutResult::too_large);
    ASSERT_NE(cache.find("k", t0), nullptr);
    EXPECT_EQ(cache.find("k", t0)->value(), "old");
}

TEST(LruCache, ExpiredEntriesDisappearOnAccess) {
    StringCache cache(10);
    cache.put("k", "v", t0 + 10ms);
    EXPECT_NE(cache.find("k", t0 + 9ms), nullptr);
    EXPECT_EQ(cache.peek("k", t0 + 10ms), nullptr);
    EXPECT_EQ(cache.size(), 1u) << "peek is const and must not remove";
    EXPECT_EQ(cache.find("k", t0 + 10ms), nullptr);
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.stats().expirations, 1u);
}

TEST(LruCache, EvictExpiredRemovesOnlyDeadKeysInDeadlineOrder) {
    StringCache cache(10);
    cache.put("late", "v", t0 + 30ms);
    cache.put("early", "v", t0 + 10ms);
    cache.put("forever", "v");
    cache.put("middle", "v", t0 + 20ms);

    EXPECT_EQ(cache.evict_expired(t0 + 25ms, 1), 1u);  // limit respected: only the earliest
    EXPECT_EQ(cache.peek("early", t0), nullptr);
    EXPECT_NE(cache.peek("middle", t0), nullptr);

    EXPECT_EQ(cache.evict_expired(t0 + 25ms), 1u);
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_EQ(cache.keys_with_expiry(), 1u);
}

TEST(LruCache, SetExpiryAddsChangesAndClearsDeadlines) {
    StringCache cache(10);
    cache.put("k", "v");
    auto* e = cache.find("k", t0);
    cache.set_expiry(*e, t0 + 5ms);
    EXPECT_EQ(e->expires_at(), t0 + 5ms);
    cache.set_expiry(*e, t0 + 50ms);
    EXPECT_EQ(cache.keys_with_expiry(), 1u);
    cache.set_expiry(*e, std::nullopt);
    EXPECT_FALSE(e->expires_at());
    EXPECT_EQ(cache.evict_expired(t0 + 1h), 0u);
}

TEST(LruCache, ReplacingAKeyResetsItsTtl) {
    StringCache cache(10);
    cache.put("k", "v", t0 + 5ms);
    cache.put("k", "v2");
    EXPECT_EQ(cache.keys_with_expiry(), 0u);
    EXPECT_NE(cache.find("k", t0 + 1h), nullptr);
}

TEST(LruCache, ReweighAfterInPlaceMutation) {
    LruCache<std::string, std::string, SizeWeigher> cache(20);
    cache.put("a", "12345");  // 6
    cache.put("b", "12345");  // 6
    auto* b = cache.find("b", t0);
    b->value().append(10, 'x');  // b now weighs 16; total 22 > 20
    EXPECT_TRUE(cache.reweigh(*b));
    EXPECT_EQ(cache.find("a", t0), nullptr) << "the other entry is evicted, not the one being updated";
    EXPECT_EQ(cache.weight(), 16u);

    b->value().append(10, 'x');  // 26 > capacity on its own
    EXPECT_FALSE(cache.reweigh(*b));
    EXPECT_EQ(cache.size(), 0u);
    EXPECT_EQ(cache.weight(), 0u);
}

TEST(LruCache, ForEachVisitsLiveEntriesMostRecentFirst) {
    StringCache cache(10);
    cache.put("a", "1");
    cache.put("b", "2", t0 + 1ms);
    cache.put("c", "3");
    std::string order;
    cache.for_each(t0 + 5ms, [&](const StringCache::Entry& e) { order += e.key(); });
    EXPECT_EQ(order, "ca");
}

TEST(LruCache, MovedCacheKeepsWorking) {
    StringCache source(2);
    source.put("a", "1", t0 + 10ms);
    source.put("b", "2");
    StringCache cache = std::move(source);  // stored iterators must survive the move

    EXPECT_EQ(value_of(cache, "a"), "1");
    cache.put("c", "3");  // evicts b (a was just used)
    EXPECT_EQ(cache.find("b", t0), nullptr);
    EXPECT_EQ(cache.evict_expired(t0 + 1h), 1u);
    EXPECT_EQ(cache.size(), 1u);
}

TEST(LruCache, ShrinkingCapacityEvicts) {
    StringCache cache(5);
    for (char c = 'a'; c < 'f'; ++c) cache.put(std::string(1, c), "v");
    cache.set_capacity(2);
    EXPECT_EQ(cache.size(), 2u);
    EXPECT_NE(cache.peek("e", t0), nullptr);
    EXPECT_NE(cache.peek("d", t0), nullptr);
}

// ---------------------------------------------------- exception safety

namespace {

// Number of hash calls to allow before throwing; -1 = never throw.
int g_hashes_before_throw = -1;

struct Key {
    int id;
    friend bool operator==(const Key&, const Key&) = default;
};

struct ThrowingHash {
    std::size_t operator()(const Key& k) const {
        if (g_hashes_before_throw >= 0 && g_hashes_before_throw-- == 0) throw std::runtime_error("hash failure");
        return std::hash<int>{}(k.id);
    }
};

}  // namespace

TEST(LruCache, FailedInsertLeavesCacheUnchanged) {
    LruCache<Key, std::string, UnitWeigher, ThrowingHash> cache(10);
    cache.put(Key{1}, "one");

    // Let put()'s initial lookup succeed, then fail while inserting into the
    // index, after the node was already added to the recency list. That is
    // exactly the half-done state the rollback has to undo.
    g_hashes_before_throw = 1;
    EXPECT_THROW(cache.put(Key{2}, "two"), std::runtime_error);
    g_hashes_before_throw = -1;

    EXPECT_EQ(cache.size(), 1u);
    EXPECT_EQ(cache.weight(), 1u);
    EXPECT_NE(cache.find(Key{1}, t0), nullptr);
    EXPECT_EQ(cache.find(Key{2}, t0), nullptr);
    cache.put(Key{2}, "two");  // and it is still fully usable
    EXPECT_EQ(cache.size(), 2u);
}

// ------------------------------------------------------- shared budget

TEST(LruCacheBudget, CachesSharingABudgetStayUnderItTogether) {
    MemoryBudget budget(4);
    StringCache a(100, {}, &budget);
    StringCache b(100, {}, &budget);
    a.put("a1", "v");
    a.put("a2", "v");
    a.put("a3", "v");
    b.put("b1", "v");
    EXPECT_EQ(budget.used.load(), 4u);

    b.put("b2", "v");  // over budget: b evicts its own LRU entry, never a's
    EXPECT_EQ(b.size(), 1u);
    EXPECT_NE(b.peek("b2", t0), nullptr) << "the entry just written is never the victim";
    EXPECT_EQ(a.size(), 3u);
    EXPECT_EQ(budget.used.load(), 4u);

    StringCache c(100, {}, &budget);
    c.put("c1", "v");  // c has nothing else to give up, so the budget stays over by one...
    EXPECT_TRUE(budget.exceeded());
    EXPECT_EQ(c.size(), 1u);
    EXPECT_EQ(a.trim(), 1u);  // ...until another cache trims
    EXPECT_FALSE(budget.exceeded());
}

TEST(LruCacheBudget, EveryByteIsReturnedToTheBudget) {
    MemoryBudget budget(1000);
    {
        StringCache a(100, {}, &budget);
        a.put("x", "v");
        a.put("y", "v", t0 + 1ms);
        StringCache moved = std::move(a);  // must not double-count or double-release
        moved.put("z", "v");
        (void)moved.find("y", t0 + 1h);  // expires
        EXPECT_EQ(budget.used.load(), 2u);
        StringCache other(100, {}, &budget);
        other.put("w", "v");
        other = std::move(moved);  // releases "w", takes over x and z
        EXPECT_EQ(budget.used.load(), 2u);
    }
    EXPECT_EQ(budget.used.load(), 0u);
}
