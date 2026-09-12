#include "test_framework.hpp"
#include "Core/Context/EmbeddingCache.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(EmbeddingCache_PutGet_RoundTrips) {
    EmbeddingCache cache;
    cache.Put("Player::Attack", {0.1F, 0.2F, 0.3F}, "hash-v1");

    const auto result = cache.Get("Player::Attack", "hash-v1");
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(result->size() == 3);
    AISTUDIO_EXPECT((*result)[0] == 0.1F);
}

AISTUDIO_TEST(EmbeddingCache_Get_StaleContentHash_IsMiss) {
    EmbeddingCache cache;
    cache.Put("Player::Attack", {0.1F, 0.2F}, "hash-v1");

    // The symbol's source changed since the embedding was computed --
    // must miss rather than return a now-inaccurate vector.
    const auto result = cache.Get("Player::Attack", "hash-v2");
    AISTUDIO_EXPECT(!result.has_value());
}

AISTUDIO_TEST(EmbeddingCache_Get_MissingKey_ReturnsNullopt) {
    EmbeddingCache cache;
    AISTUDIO_EXPECT(!cache.Get("missing", "hash-v1").has_value());
}

AISTUDIO_TEST(EmbeddingCache_Invalidate_RemovesEntry) {
    EmbeddingCache cache;
    cache.Put("k", {1.0F}, "hash-v1");
    cache.Invalidate("k");

    AISTUDIO_EXPECT(!cache.Get("k", "hash-v1").has_value());
}

AISTUDIO_TEST(EmbeddingCache_Clear_RemovesEverything) {
    EmbeddingCache cache;
    cache.Put("a", {1.0F}, "hash-v1");
    cache.Put("b", {2.0F}, "hash-v1");
    cache.Clear();

    AISTUDIO_EXPECT(cache.Stats().size == 0);
}

AISTUDIO_TEST(EmbeddingCache_Stats_TracksHitsAndMisses) {
    EmbeddingCache cache;
    cache.Put("k", {1.0F}, "hash-v1");

    const auto hit = cache.Get("k", "hash-v1");
    const auto miss = cache.Get("missing", "hash-v1");
    AISTUDIO_EXPECT(hit.has_value());
    AISTUDIO_EXPECT(!miss.has_value());

    const auto stats = cache.Stats();
    AISTUDIO_EXPECT(stats.hits == 1);
    AISTUDIO_EXPECT(stats.misses == 1);
}
