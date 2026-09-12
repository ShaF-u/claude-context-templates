#include "test_framework.hpp"
#include "Core/Cache/Cache.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(Cache_PutGet_RoundTrips) {
    Cache<std::string> cache;
    cache.Put("k1", "v1");

    const auto result = cache.Get("k1");
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(*result == "v1");
    AISTUDIO_EXPECT(cache.Stats().hits == 1);
}

AISTUDIO_TEST(Cache_Get_MissingKey_ReturnsNulloptAndCountsMiss) {
    Cache<int> cache;
    const auto result = cache.Get("missing");
    AISTUDIO_EXPECT(!result.has_value());
    AISTUDIO_EXPECT(cache.Stats().misses == 1);
}

AISTUDIO_TEST(Cache_Contains_ReflectsPresence) {
    Cache<int> cache;
    AISTUDIO_EXPECT(!cache.Contains("k"));
    cache.Put("k", 1);
    AISTUDIO_EXPECT(cache.Contains("k"));
}

AISTUDIO_TEST(Cache_Ttl_ExpiresAfterDuration) {
    Cache<int>::TimePoint fake_now = Cache<int>::Clock::now();
    Cache<int> cache([&fake_now] { return fake_now; });

    Cache<int>::PutOptions options;
    options.ttl = std::chrono::milliseconds(100);
    cache.Put("k", 42, options);

    AISTUDIO_EXPECT(cache.Get("k").has_value());

    fake_now += std::chrono::milliseconds(200);

    const auto expired_result = cache.Get("k");
    AISTUDIO_EXPECT(!expired_result.has_value());
    AISTUDIO_EXPECT(cache.Stats().evictions == 1);
    AISTUDIO_EXPECT(!cache.Contains("k"));
}

AISTUDIO_TEST(Cache_Ttl_NulloptNeverExpires) {
    Cache<int>::TimePoint fake_now = Cache<int>::Clock::now();
    Cache<int> cache([&fake_now] { return fake_now; });

    cache.Put("k", 1); // no ttl
    fake_now += std::chrono::hours(24 * 365);

    AISTUDIO_EXPECT(cache.Get("k").has_value());
}

AISTUDIO_TEST(Cache_Get_VersionMismatch_IsTreatedAsMiss) {
    Cache<std::string> cache;
    Cache<std::string>::PutOptions options;
    options.version = "hash-v1";
    cache.Put("k", "content", options);

    AISTUDIO_EXPECT(cache.Get("k", "hash-v1").has_value());
    AISTUDIO_EXPECT(!cache.Get("k", "hash-v2").has_value());
    // The stale entry is evicted, so a subsequent lookup with the
    // original version also misses.
    AISTUDIO_EXPECT(!cache.Get("k", "hash-v1").has_value());
}

AISTUDIO_TEST(Cache_Invalidate_RemovesEntryAndCounts) {
    Cache<int> cache;
    cache.Put("k", 1);
    cache.Invalidate("k");

    AISTUDIO_EXPECT(!cache.Contains("k"));
    AISTUDIO_EXPECT(cache.Stats().invalidations == 1);
}

AISTUDIO_TEST(Cache_Invalidate_MissingKey_DoesNotCount) {
    Cache<int> cache;
    cache.Invalidate("missing");
    AISTUDIO_EXPECT(cache.Stats().invalidations == 0);
}

AISTUDIO_TEST(Cache_InvalidateByDependency_CascadesToDependents) {
    Cache<std::string> cache;
    Cache<std::string>::PutOptions options;
    options.dependencies = {"Player.cpp"};
    cache.Put("ast:Player.cpp", "ast-data", options);
    cache.Put("symbol:Player::Attack", "symbol-data", options);
    cache.Put("unrelated", "other-data");

    cache.InvalidateByDependency("Player.cpp");

    AISTUDIO_EXPECT(!cache.Contains("ast:Player.cpp"));
    AISTUDIO_EXPECT(!cache.Contains("symbol:Player::Attack"));
    AISTUDIO_EXPECT(cache.Contains("unrelated"));
    AISTUDIO_EXPECT(cache.Stats().invalidations == 2);
}

AISTUDIO_TEST(Cache_Clear_RemovesEverything) {
    Cache<int> cache;
    cache.Put("a", 1);
    cache.Put("b", 2);
    cache.Clear();

    AISTUDIO_EXPECT(cache.Stats().size == 0);
    AISTUDIO_EXPECT(!cache.Contains("a"));
    AISTUDIO_EXPECT(!cache.Contains("b"));
}

AISTUDIO_TEST(Cache_Stats_SizeReflectsCurrentEntries) {
    Cache<int> cache;
    cache.Put("a", 1);
    cache.Put("b", 2);
    AISTUDIO_EXPECT(cache.Stats().size == 2);

    cache.Invalidate("a");
    AISTUDIO_EXPECT(cache.Stats().size == 1);
}

AISTUDIO_TEST(Cache_Put_OverwritesExistingEntry) {
    Cache<std::string> cache;
    cache.Put("k", "v1");
    cache.Put("k", "v2");

    const auto result = cache.Get("k");
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(*result == "v2");
    AISTUDIO_EXPECT(cache.Stats().size == 1);
}
