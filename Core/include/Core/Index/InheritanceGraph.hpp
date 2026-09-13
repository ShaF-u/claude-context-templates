#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Index/InheritanceEdge.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace aistudio::core {

// Aggregates base-class relationships across every source file under a
// project root — docs/ROADMAP.md Phase 3 "Code Intelligence" >
// "Inheritance Graph". Built on FileScanner (reuses its ignore rules,
// same as SymbolIndex/IncludeGraph/CallGraph) + InheritanceExtractor's
// tree-sitter-based per-file parsing, with the same Cache<T>-backed
// per-file extraction (keyed by path, versioned on
// FileMetadata::content_hash) so unchanged files skip re-parsing on
// rebuild.
//
// Unlike CallGraph::Callers, both Bases() and Derived() are plain exact
// name matches — a base class reference has no member-access-vs-
// qualified-call ambiguity to hedge against, tree-sitter already hands
// InheritanceExtractor the base's plain identifier directly. Has no
// dependency on SymbolIndex, for the same reason CallGraph doesn't: a
// caller that wants to resolve a class name to the file/Symbol that
// defines it can cross-reference SymbolIndex itself.
//
// Thread-safe: every public method locks an internal mutex around its
// access to edges_ (Cache<T> already locks its own state internally) --
// same reasoning as SymbolIndex (docs/ROADMAP.md "File Watcher").
class InheritanceGraph {
public:
    // `extra_ignore_patterns`: see SymbolIndex::Build's identical parameter.
    Result<void> Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns = {});

    // Re-extracts just `path`'s own inheritance edges and replaces them
    // in this index, without re-scanning the rest of the project --
    // docs/ROADMAP.md "Incremental Reload". Like CallGraph's own
    // UpdateFile(), no cross-file re-resolution is needed: derived_name/
    // base_name are plain text with no dependency on any other file's
    // content. No-op (Ok) if `path` isn't a recognized source file
    // extension.
    Result<void> UpdateFile(const std::string& root, const std::string& path);

    // Removes every edge whose derived_file is `path` and evicts its
    // Cache<T> entry. No-op (still Ok) if `path` had no indexed edges.
    Result<void> RemoveFile(const std::string& path);

    // Direct base classes of `derived_name` (does not recurse into the
    // bases' own bases — see ImpactAnalyzer for the transitive-closure
    // pattern used elsewhere in this project, not duplicated here).
    [[nodiscard]] std::vector<InheritanceEdge> Bases(const std::string& derived_name) const;
    // Direct subclasses of `base_name`.
    [[nodiscard]] std::vector<InheritanceEdge> Derived(const std::string& base_name) const;

    [[nodiscard]] std::vector<InheritanceEdge> AllEdges() const;
    [[nodiscard]] std::size_t Size() const;
    // Cache<T>::Stats() locks Cache's own internal mutex, not this
    // class's -- no lock needed here, there's nothing of ours to protect.
    [[nodiscard]] CacheStats Stats() const { return cache_.Stats(); }

private:
    [[nodiscard]] static bool IsSourceFile(const std::string& path);

    mutable std::mutex mutex_; // guards edges_ only -- cache_ is separately thread-safe
    std::vector<InheritanceEdge> edges_;
    Cache<std::vector<InheritanceEdge>> cache_;
};

} // namespace aistudio::core
