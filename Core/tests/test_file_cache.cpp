#include "test_framework.hpp"
#include "Core/Project/FileCache.hpp"

using namespace aistudio::core;

namespace {
FileMetadata MakeMetadata(std::string path, std::string hash) {
    FileMetadata metadata;
    metadata.path = std::move(path);
    metadata.content_hash = std::move(hash);
    return metadata;
}
} // namespace

AISTUDIO_TEST(FileCache_PutGet_RoundTrips) {
    FileCache cache;
    const auto metadata = MakeMetadata("src/main.cpp", "hash-v1");
    cache.Put(metadata, "int main() {}");

    const auto result = cache.Get(metadata);
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(*result == "int main() {}");
}

AISTUDIO_TEST(FileCache_Get_StaleHash_IsMiss) {
    FileCache cache;
    cache.Put(MakeMetadata("src/main.cpp", "hash-v1"), "old content");

    // Same path, different content_hash -> the file changed on disk since
    // it was cached, so this must miss rather than return stale content.
    const auto result = cache.Get(MakeMetadata("src/main.cpp", "hash-v2"));
    AISTUDIO_EXPECT(!result.has_value());
}

AISTUDIO_TEST(FileCache_Invalidate_RemovesEntry) {
    FileCache cache;
    const auto metadata = MakeMetadata("src/main.cpp", "hash-v1");
    cache.Put(metadata, "content");
    cache.Invalidate("src/main.cpp");

    AISTUDIO_EXPECT(!cache.Get(metadata).has_value());
}

AISTUDIO_TEST(FileCache_Stats_TracksHitsAndMisses) {
    FileCache cache;
    const auto metadata = MakeMetadata("a.txt", "hash-v1");
    cache.Put(metadata, "content");

    const auto hit_result = cache.Get(metadata);
    const auto miss_result = cache.Get(MakeMetadata("missing.txt", "hash-v1"));
    AISTUDIO_EXPECT(hit_result.has_value());
    AISTUDIO_EXPECT(!miss_result.has_value());

    const auto stats = cache.Stats();
    AISTUDIO_EXPECT(stats.hits == 1);
    AISTUDIO_EXPECT(stats.misses == 1);
}
