#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Index/CallEdge.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace aistudio::core {

// Aggregates call sites across every source file under a project root —
// docs/ROADMAP.md Phase 3 "Code Intelligence" > "Caller / Callee Graph".
// Built on FileScanner (reuses its ignore rules, same as SymbolIndex/
// IncludeGraph) + CallExtractor's tree-sitter-based per-file parsing,
// with the same Cache<T>-backed per-file extraction (keyed by path,
// versioned on FileMetadata::content_hash) so unchanged files skip
// re-parsing on rebuild.
//
// Deliberately has no dependency on SymbolIndex: `CallEdge::caller_name`
// happens to use the same qualified-name convention SymbolExtractor
// gives a function (so the two naturally line up for a caller that wants
// to cross-reference them), but CallGraph itself only ever compares
// edges against each other. Callees() is an exact match on caller_name.
// Callers() is necessarily heuristic — since no type resolution is
// attempted (CallEdge::callee_text is call-site text, not a resolved
// symbol), it matches a target name against both the full callee_text
// and its trailing identifier segment (so querying "Select" finds a call
// site written as "selector.Select(...)", not just "Select(...)").
//
// Thread-safe: every public method locks an internal mutex around its
// access to edges_ (Cache<T> already locks its own state internally) --
// same reasoning as SymbolIndex (docs/ROADMAP.md "File Watcher").
class CallGraph {
public:
    // `extra_ignore_patterns`: see SymbolIndex::Build's identical parameter.
    Result<void> Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns = {});

    // Re-extracts just `path`'s own call edges and replaces them in this
    // index, without re-scanning the rest of the project --
    // docs/ROADMAP.md "Incremental Reload". Unlike IncludeGraph's own
    // UpdateFile(), no re-resolution of OTHER files' edges is needed:
    // caller_name/callee_text are plain text with no cross-file
    // resolution step (see the class comment above), so one file's
    // edges never depend on any other file's content. No-op (Ok) if
    // `path` isn't a recognized source file extension.
    Result<void> UpdateFile(const std::string& root, const std::string& path);

    // Removes every edge whose caller_file is `path` and evicts its
    // Cache<T> entry. No-op (still Ok) if `path` had no indexed edges.
    Result<void> RemoveFile(const std::string& path);

    [[nodiscard]] std::vector<CallEdge> Callees(const std::string& caller_name) const;
    [[nodiscard]] std::vector<CallEdge> Callers(const std::string& callee_name) const;

    [[nodiscard]] std::vector<CallEdge> AllEdges() const;
    [[nodiscard]] std::size_t Size() const;
    // Cache<T>::Stats() locks Cache's own internal mutex, not this
    // class's -- no lock needed here, there's nothing of ours to protect.
    [[nodiscard]] CacheStats Stats() const { return cache_.Stats(); }

private:
    [[nodiscard]] static bool IsSourceFile(const std::string& path);

    mutable std::mutex mutex_; // guards edges_ only -- cache_ is separately thread-safe
    std::vector<CallEdge> edges_;
    Cache<std::vector<CallEdge>> cache_;
};

} // namespace aistudio::core
