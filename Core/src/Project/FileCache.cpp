#include "Core/Project/FileCache.hpp"

namespace aistudio::core {

void FileCache::Put(const FileMetadata& metadata, std::string content) {
    Cache<std::string>::PutOptions options;
    options.version = metadata.content_hash;
    cache_.Put(metadata.path, std::move(content), options);
}

std::optional<std::string> FileCache::Get(const FileMetadata& metadata) {
    return cache_.Get(metadata.path, metadata.content_hash);
}

void FileCache::Invalidate(const std::string& relative_path) {
    cache_.Invalidate(relative_path);
}

CacheStats FileCache::Stats() const {
    return cache_.Stats();
}

} // namespace aistudio::core
