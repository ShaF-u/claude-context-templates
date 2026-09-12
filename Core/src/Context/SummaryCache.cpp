#include "Core/Context/SummaryCache.hpp"

#include "Core/Project/FileScanner.hpp" // HashContent

namespace aistudio::core {

namespace {
std::string VersionFor(const ContextItem& item, std::int64_t target_tokens) {
    // ContextCompressor::Compress() branches on item.estimated_tokens
    // (its `estimated_tokens <= target_tokens` early-return gate) in
    // addition to item.content and target_tokens -- estimated_tokens is
    // caller-set (see ContextItem.hpp), not derived from content, so two
    // calls with identical id/content/target_tokens but different
    // estimated_tokens can legitimately produce different outputs (one
    // Raw, one Summary). It must be part of the version fingerprint too,
    // or the second caller gets a stale cache hit from the first.
    return HashContent(item.content) + ":" + std::to_string(target_tokens) + ":" +
           std::to_string(item.estimated_tokens);
}
} // namespace

ContextItem SummaryCache::GetOrCompress(const ContextCompressor& compressor, const ContextItem& item,
                                         std::int64_t target_tokens) {
    const std::string version = VersionFor(item, target_tokens);
    if (auto cached = cache_.Get(item.id, version); cached.has_value()) {
        return *cached;
    }

    ContextItem compressed = compressor.Compress(item, target_tokens);
    Cache<ContextItem>::PutOptions options;
    options.version = version;
    cache_.Put(item.id, compressed, options);
    return compressed;
}

void SummaryCache::Invalidate(const std::string& item_id) {
    cache_.Invalidate(item_id);
}

void SummaryCache::Clear() {
    cache_.Clear();
}

CacheStats SummaryCache::Stats() const {
    return cache_.Stats();
}

} // namespace aistudio::core
