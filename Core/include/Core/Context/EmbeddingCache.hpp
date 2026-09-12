#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"

#include <optional>
#include <string>
#include <vector>

namespace aistudio::core {

// Storage/invalidation layer for embedding vectors — docs/ROADMAP.md
// Phase 2 "Context Cache" > "Embedding Cache". Deliberately a cache
// PRIMITIVE only: nothing in this codebase computes an embedding yet
// (Phase 2 "Semantic retrieval"/"Semantic summary" and Phase 3's
// "Semantic Search" are text-ranking based — SymbolSearch/KeywordSearch —
// not vector embeddings; there is no ML/embedding model wired in). Adding
// a fake or placeholder embedding *algorithm* just to have something to
// cache would be exactly the kind of premature, hollow implementation
// AGENT.md #14/#16 warn against ("実装速度" is the LOWEST priority; a
// component with no real producer can't be tested against real
// behavior). What this class DOES provide now is the piece that's
// actually reusable regardless of which embedding model eventually
// produces the vectors: keyed storage with correct invalidation, so
// wiring in a real embedding generator later is "call Put() after
// computing," not "design a cache from scratch."
//
// Same versioned-by-content-hash pattern every other cache in this
// project uses (FileCache / SymbolIndex / IncludeGraph / AstIndex, all
// built on Cache<T> and versioned via Project::HashContent) — the AGENT.md
// #7 requirement that Context/AST/Embedding Cache specifically must never
// serve stale data after the source content changes. Cache<T>::Get(key,
// version) already treats a version mismatch as a miss and evicts the
// stale entry, so this class only has to plumb that through with
// Embedding-shaped types; it invents no new invalidation mechanism.
class EmbeddingCache {
public:
    using Embedding = std::vector<float>;

    // `key` identifies WHAT was embedded (e.g. a Symbol id, a file path,
    // a chunk id) — the caller's choice, this class doesn't interpret it.
    // `content_hash` identifies the exact content the embedding was
    // computed FROM (e.g. Project::HashContent() of the source text, or
    // any other string a future embedding pipeline uses as its own
    // change-detection hash) and becomes the Cache<T> version: a later
    // Get() for the same key with a different content_hash is a miss,
    // exactly like FileCache treating a changed FileMetadata::content_hash
    // as a miss.
    void Put(const std::string& key, Embedding embedding, const std::string& content_hash);

    // Returns the cached embedding only if `content_hash` matches what it
    // was stored with; nullopt on miss (absent, or the source content
    // changed since).
    [[nodiscard]] std::optional<Embedding> Get(const std::string& key, const std::string& content_hash);

    void Invalidate(const std::string& key);
    void Clear();
    [[nodiscard]] CacheStats Stats() const;

private:
    Cache<Embedding> cache_;
};

} // namespace aistudio::core
