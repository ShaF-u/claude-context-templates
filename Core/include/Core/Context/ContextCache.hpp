#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Context/ContextItem.hpp"
#include "Core/Context/ContextRetriever.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aistudio::core {

// Memoizes ContextRetriever::Retrieve(intent) — docs/ROADMAP.md Phase 2
// "Context Cache" (distinct from the section heading of the same name;
// this is specifically the checklist item asking to cache assembled
// ContextRetriever results, not File/AST/Symbol Cache which already
// exist inside the indexes themselves).
//
// Invalidation design (AGENT.md #7 — "元データの変更時に古いデータが残
// らないようにしてください", called out by name for Context Cache):
// unlike FileCache/SymbolIndex/AstIndex, a Retrieve() result has no
// single content-hash to version against. It's assembled from up to five
// independent live sources (see ContextRetriever::Retrieve): a Symbol
// match (SymbolIndex, static per process in MCP mode), an Include edge
// (IncludeGraph, same), a File path match (FileScanner::Scan() —
// RE-SCANS THE ENTIRE PROJECT on every single call, regardless of
// intent), a Keyword match (KeywordSearch — also a full re-scan), and a
// Backend's ProvideContext() (arbitrary — e.g. GitBackend reads live git
// state). Because File/Keyword retrieval already re-read the whole
// project on every call, ANY changed, added, or removed file is a
// potential input to EVERY cached intent, not just the ones that
// happened to touch it before — the correct dependency scope for a
// cached Retrieve() result is genuinely "the whole project", not a
// per-file dependency list. Cache<T>::InvalidateByDependency() (the
// per-key dependency mechanism EmbeddingCache-adjacent caches use) would
// therefore add complexity without adding real precision here: tracking
// per-intent dependencies could only ever be an approximation of "the
// whole project changed", so this class deliberately doesn't try —
// InvalidateAll() discarding every cached intent at once is the honestly
// correct granularity, not a shortcut. (Mechanically this is a
// generation bump, not Cache<T>::Clear() -- see GetOrRetrieve()'s own
// comment; the effect on what gets served is the same.)
//
// The trigger for InvalidateAll() is the real "FileChanged" EventBus
// event (AGENT.md #5) FileWatcher already publishes — see the caller,
// not this class: like every other EventBus subscription in this
// codebase (FileWatcher-driven index updates, ContextAudit persistence —
// see Core/src/main.cpp), subscription/unsubscription happens at the
// bootstrap call site, not self-managed inside the class, so this class
// stays trivially unit-testable without EventBus involved at all. No TTL
// is used anywhere in this class — the explicit, event-driven
// InvalidateAll() is the sole invalidation mechanism, exactly what
// AGENT.md #7 asks for instead of "an easy but wrong" TTL-only cache.
class ContextCache {
public:
    // Returns a cached result for `intent` if one exists for this exact
    // `retriever` and its current active document; otherwise calls
    // retriever.Retrieve(intent), caches the result, and returns it.
    //
    // Cache key (docs/ROADMAP.md CE-4, resolving the "code review,
    // 2026-09-05" known limitation this comment used to describe):
    // `&retriever`'s own identity (closing the "different retriever, same
    // intent string" leak the old intent-only key had) + `intent` +
    // CurrentActiveDocumentPath() (Active File Bias changes a result's
    // priority based on this, so a stale answer from before the active
    // document changed must not be served as a hit for the new one).
    // project_root/firewall aren't folded in separately: every current
    // call site already binds one ContextRetriever to one ContextCache
    // for its whole process lifetime, so `&retriever` alone already
    // implies a fixed project_root/firewall too.
    [[nodiscard]] std::vector<ContextItem> GetOrRetrieve(const ContextRetriever& retriever, const std::string& intent);

    // Discards every cached intent — the intended, coarse-grained
    // invalidation call (see class comment for why coarse is correct
    // here), meant to be wired to the "FileChanged" event. Implemented as
    // an internal generation bump (see CurrentGeneration()) rather than
    // Cache<T>::Clear(): stale entries are never served again (a version
    // mismatch is a Cache<T> miss, docs/ROADMAP.md CE-4), they just aren't
    // reclaimed from memory until next looked up -- the same "invalidated,
    // not necessarily freed" behavior every other Version-based cache in
    // this codebase (FileCache, SymbolIndex) already has.
    void InvalidateAll();

    // Discards one cached intent for this `retriever`+its current active
    // document. Exists for callers that know a specific intent's answer
    // is stale without needing InvalidateAll()'s whole-project reasoning
    // to apply to their use case.
    void Invalidate(const ContextRetriever& retriever, const std::string& intent);

    [[nodiscard]] CacheStats Stats() const;

private:
    Cache<std::vector<ContextItem>> cache_;
    std::int64_t generation_ = 0;
};

} // namespace aistudio::core
