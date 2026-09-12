#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Project/FileMetadata.hpp"

#include <optional>
#include <string>

namespace aistudio::core {

// Caches file content keyed by relative path, using FileMetadata's
// content_hash as the Cache<T> version — a changed file (different hash)
// is automatically a miss instead of serving stale content. Built on
// Cache<T> (Phase 1 "Cache Foundation") rather than a bespoke cache, per
// docs/ROADMAP.md Phase 2 "File Cache".
class FileCache {
public:
    void Put(const FileMetadata& metadata, std::string content);
    [[nodiscard]] std::optional<std::string> Get(const FileMetadata& metadata);
    void Invalidate(const std::string& relative_path);
    [[nodiscard]] CacheStats Stats() const;

private:
    Cache<std::string> cache_;
};

} // namespace aistudio::core
