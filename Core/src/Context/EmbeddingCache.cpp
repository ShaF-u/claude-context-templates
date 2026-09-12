#include "Core/Context/EmbeddingCache.hpp"

namespace aistudio::core {

void EmbeddingCache::Put(const std::string& key, Embedding embedding, const std::string& content_hash) {
    Cache<Embedding>::PutOptions options;
    options.version = content_hash;
    cache_.Put(key, std::move(embedding), options);
}

std::optional<EmbeddingCache::Embedding> EmbeddingCache::Get(const std::string& key, const std::string& content_hash) {
    return cache_.Get(key, content_hash);
}

void EmbeddingCache::Invalidate(const std::string& key) {
    cache_.Invalidate(key);
}

void EmbeddingCache::Clear() {
    cache_.Clear();
}

CacheStats EmbeddingCache::Stats() const {
    return cache_.Stats();
}

} // namespace aistudio::core
