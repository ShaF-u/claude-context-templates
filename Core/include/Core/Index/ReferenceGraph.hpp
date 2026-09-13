#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Index/ReferenceEdge.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace aistudio::core {

// Aggregates type-reference sites across every source file under a
// project root — docs/ROADMAP.md Phase 3 "Code Intelligence" >
// "Reference Graph". Built on FileScanner (reuses its ignore rules,
// same as SymbolIndex/IncludeGraph/CallGraph/InheritanceGraph) +
// ReferenceExtractor's tree-sitter-based per-file parsing, with the
// same Cache<T>-backed per-file extraction (keyed by path, versioned on
// FileMetadata::content_hash) so unchanged files skip re-parsing on
// rebuild. Has no dependency on SymbolIndex, for the same reason
// CallGraph/InheritanceGraph don't: a caller wanting to resolve a type
// name to the file/Symbol that defines it can cross-reference
// SymbolIndex itself. References() is a plain exact-name match — a
// type_identifier's text has no member-access-vs-qualified ambiguity
// the way a call site's callee expression does.
//
// Thread-safe: every public method locks an internal mutex around its
// access to edges_ (Cache<T> already locks its own state internally) --
// same reasoning as SymbolIndex (docs/ROADMAP.md "File Watcher").
class ReferenceGraph {
public:
    // `extra_ignore_patterns`: see SymbolIndex::Build's identical parameter.
    Result<void> Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns = {});

    // Re-extracts just `path`'s own type-reference edges and replaces
    // them in this index, without re-scanning the rest of the project --
    // docs/ROADMAP.md "Incremental Reload". Like CallGraph/
    // InheritanceGraph's own UpdateFile(), no cross-file re-resolution is
    // needed: type_name is plain text with no dependency on any other
    // file's content. No-op (Ok) if `path` isn't a recognized source
    // file extension.
    Result<void> UpdateFile(const std::string& root, const std::string& path);

    // Removes every edge whose referencing_file is `path` and evicts its
    // Cache<T> entry. No-op (still Ok) if `path` had no indexed edges.
    Result<void> RemoveFile(const std::string& path);

    [[nodiscard]] std::vector<ReferenceEdge> References(const std::string& type_name) const;
    [[nodiscard]] std::vector<ReferenceEdge> AllEdges() const;
    [[nodiscard]] std::size_t Size() const;
    // Cache<T>::Stats() locks Cache's own internal mutex, not this
    // class's -- no lock needed here, there's nothing of ours to protect.
    [[nodiscard]] CacheStats Stats() const { return cache_.Stats(); }

private:
    [[nodiscard]] static bool IsSourceFile(const std::string& path);

    mutable std::mutex mutex_; // guards edges_ only -- cache_ is separately thread-safe
    std::vector<ReferenceEdge> edges_;
    Cache<std::vector<ReferenceEdge>> cache_;
};

} // namespace aistudio::core
