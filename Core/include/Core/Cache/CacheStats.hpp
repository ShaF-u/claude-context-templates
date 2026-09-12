#pragma once

#include <cstddef>
#include <cstdint>

namespace aistudio::core {

// Observability for a Cache instance (AGENT.md #9 — "何が起きたかわからない
// 状態を作らない"). Every Cache<T> exposes these via Stats() so callers
// (Context Budget UI, Cache monitoring) can see hit rate without
// instrumenting call sites themselves.
struct CacheStats {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t evictions = 0;      // entries removed for being expired or version/hash-stale
    std::uint64_t invalidations = 0;  // entries removed via explicit Invalidate()/InvalidateByDependency()
    std::size_t size = 0;             // current entry count
};

} // namespace aistudio::core
