#include "test_framework.hpp"
#include "Core/Context/SummaryCache.hpp"

using namespace aistudio::core;

namespace {
ContextItem MakeItem(std::string id, std::string content) {
    ContextItem item;
    item.id = std::move(id);
    item.content = std::move(content);
    item.estimated_tokens = EstimateTokens(item.content);
    return item;
}

std::string MakeOversizedContent() {
    return "BEGIN_MARKER_" + std::string(2000, 'x') + "_END_MARKER";
}
} // namespace

AISTUDIO_TEST(SummaryCache_GetOrCompress_SameInputTwice_SecondCallIsCacheHit) {
    SummaryCache cache;
    const ContextCompressor compressor;
    const auto item = MakeItem("Player.cpp", MakeOversizedContent());

    const auto first = cache.GetOrCompress(compressor, item, /*target_tokens=*/50);
    const auto second = cache.GetOrCompress(compressor, item, /*target_tokens=*/50);

    AISTUDIO_EXPECT(first.content == second.content);
    AISTUDIO_EXPECT(first.compression == CompressionLevel::Summary);
    const auto stats = cache.Stats();
    AISTUDIO_EXPECT(stats.hits == 1);
    AISTUDIO_EXPECT(stats.misses == 1);
}

AISTUDIO_TEST(SummaryCache_GetOrCompress_ContentChanged_IsCacheMissNotStale) {
    SummaryCache cache;
    const ContextCompressor compressor;
    const auto original = MakeItem("Player.cpp", MakeOversizedContent());
    const auto original_result = cache.GetOrCompress(compressor, original, /*target_tokens=*/50);
    AISTUDIO_EXPECT(original_result.content.find('x') != std::string::npos);

    // Same id, different content (the source file changed) -- must
    // recompute rather than return the old file's summary.
    auto changed = MakeItem("Player.cpp", "BEGIN_MARKER_" + std::string(2000, 'y') + "_END_MARKER");
    const auto result = cache.GetOrCompress(compressor, changed, /*target_tokens=*/50);

    AISTUDIO_EXPECT(result.content.find('y') != std::string::npos);
    AISTUDIO_EXPECT(result.content.find('x') == std::string::npos);
    AISTUDIO_EXPECT(cache.Stats().misses == 2);
}

AISTUDIO_TEST(SummaryCache_GetOrCompress_DifferentTargetTokens_IsCacheMiss) {
    SummaryCache cache;
    const ContextCompressor compressor;
    const auto item = MakeItem("Player.cpp", MakeOversizedContent());

    const auto at_50 = cache.GetOrCompress(compressor, item, /*target_tokens=*/50);
    const auto at_100 = cache.GetOrCompress(compressor, item, /*target_tokens=*/100);

    // A tighter budget must produce a smaller (or equal) result -- if the
    // cache incorrectly served the target_tokens=50 result for the
    // target_tokens=100 request this could spuriously match, so also
    // assert the miss count directly.
    AISTUDIO_EXPECT(at_50.content.size() <= at_100.content.size());
    AISTUDIO_EXPECT(cache.Stats().misses == 2);
}

AISTUDIO_TEST(SummaryCache_Invalidate_ForcesRecompute) {
    SummaryCache cache;
    const ContextCompressor compressor;
    const auto item = MakeItem("Player.cpp", MakeOversizedContent());

    const auto first = cache.GetOrCompress(compressor, item, /*target_tokens=*/50);
    cache.Invalidate("Player.cpp");
    const auto second = cache.GetOrCompress(compressor, item, /*target_tokens=*/50);
    AISTUDIO_EXPECT(first.content == second.content);

    AISTUDIO_EXPECT(cache.Stats().misses == 2);
    AISTUDIO_EXPECT(cache.Stats().hits == 0);
}

AISTUDIO_TEST(SummaryCache_GetOrCompress_ContentAlreadyFits_ReturnsUnchangedAndStillCaches) {
    SummaryCache cache;
    const ContextCompressor compressor;
    const auto item = MakeItem("small.txt", "short");

    const auto first = cache.GetOrCompress(compressor, item, /*target_tokens=*/100);
    const auto second = cache.GetOrCompress(compressor, item, /*target_tokens=*/100);

    AISTUDIO_EXPECT(first.content == "short");
    AISTUDIO_EXPECT(second.content == "short");
    AISTUDIO_EXPECT(cache.Stats().hits == 1);
}

AISTUDIO_TEST(SummaryCache_GetOrCompress_SameContentDifferentEstimatedTokens_IsCacheMissNotStale) {
    // Regression test: Compress() gates entirely on
    // `item.estimated_tokens <= target_tokens` (an early-return that skips
    // compression), and estimated_tokens is caller-set, not derived from
    // content -- two calls with identical id/content/target_tokens but
    // different estimated_tokens must NOT collide in the cache, or the
    // second caller silently gets the first caller's stale answer to a
    // different question (see SummaryCache.hpp's class comment).
    SummaryCache cache;
    const ContextCompressor compressor;
    const std::string content = MakeOversizedContent();
    constexpr std::int64_t target_tokens = 50;

    ContextItem below_budget;
    below_budget.id = "Player.cpp";
    below_budget.content = content;
    below_budget.estimated_tokens = target_tokens - 1; // <= target_tokens: Compress() must NOT compress
    const auto unchanged_result = cache.GetOrCompress(compressor, below_budget, target_tokens);
    AISTUDIO_EXPECT(unchanged_result.content == content);
    AISTUDIO_EXPECT(unchanged_result.compression != CompressionLevel::Summary);

    ContextItem above_budget;
    above_budget.id = "Player.cpp"; // same id
    above_budget.content = content; // same content
    above_budget.estimated_tokens = target_tokens + 1; // > target_tokens: Compress() must compress
    const auto compressed_result = cache.GetOrCompress(compressor, above_budget, target_tokens); // same target_tokens

    // Must be a genuine recompute (a real Summary), not a cache hit
    // returning the first call's unchanged content.
    AISTUDIO_EXPECT(compressed_result.compression == CompressionLevel::Summary);
    AISTUDIO_EXPECT(compressed_result.content != unchanged_result.content);
    AISTUDIO_EXPECT(cache.Stats().misses == 2);
    AISTUDIO_EXPECT(cache.Stats().hits == 0);
}

AISTUDIO_TEST(SummaryCache_Clear_RemovesEverything) {
    SummaryCache cache;
    const ContextCompressor compressor;
    const auto a = cache.GetOrCompress(compressor, MakeItem("a", MakeOversizedContent()), 50);
    const auto b = cache.GetOrCompress(compressor, MakeItem("b", MakeOversizedContent()), 50);
    AISTUDIO_EXPECT(!a.content.empty());
    AISTUDIO_EXPECT(!b.content.empty());

    cache.Clear();

    AISTUDIO_EXPECT(cache.Stats().size == 0);
}
