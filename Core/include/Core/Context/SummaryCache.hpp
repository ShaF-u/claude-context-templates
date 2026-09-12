#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Context/ContextCompressor.hpp"
#include "Core/Context/ContextItem.hpp"

#include <cstdint>

namespace aistudio::core {

// Memoizes ContextCompressor::Compress() results — docs/ROADMAP.md
// Phase 2 "Context Cache" > "Summary Cache". Wraps the existing
// ContextCompressor (Phase 2 "Context Compression") rather than
// reimplementing summarization, per AGENT.md #1 ("既存機能を再利用できな
// いか確認する") — this class only adds memoization around a pure,
// already-implemented computation.
//
// Compress() is deterministic given (item.content, target_tokens,
// item.estimated_tokens) -- estimated_tokens matters too because
// Compress() early-returns `item` unchanged whenever
// `item.estimated_tokens <= target_tokens`, and estimated_tokens is a
// caller-set field (see ContextItem.hpp), not derived from content. So
// the cache key is `item.id` and the Cache<T> version is a composite of
// Project::HashContent(item.content) + target_tokens + estimated_tokens:
// if the source content changes, the caller asks for a different budget,
// OR the caller passes a different estimated_tokens for the same
// id/content/target_tokens, the stored Summary is a stale answer to a
// different question and must miss (AGENT.md #7's
// "元データの変更時に古いデータが残らないように", applied here to three
// axes of "the data" rather than just content).
// Same versioned-cache-entry pattern as FileCache/SymbolIndex/AstIndex —
// no TTL, no separate invalidation event needed, because the version
// string alone is already an exact fingerprint of everything the output
// depends on.
class SummaryCache {
public:
    // Returns a cached Summary-compressed ContextItem if `item.content` +
    // `target_tokens` + `item.estimated_tokens` exactly match a previous
    // call for the same `item.id`; otherwise compresses via `compressor`, stores the
    // result, and returns it. Note this caches Compress()'s OUTPUT keyed
    // by the INPUT content, not `item` itself — passing a different item
    // under the same id (e.g. a stale caller reusing an id) is exactly
    // the "content changed" case the version check exists to catch.
    [[nodiscard]] ContextItem GetOrCompress(const ContextCompressor& compressor, const ContextItem& item,
                                             std::int64_t target_tokens);

    void Invalidate(const std::string& item_id);
    void Clear();
    [[nodiscard]] CacheStats Stats() const;

private:
    Cache<ContextItem> cache_;
};

} // namespace aistudio::core
