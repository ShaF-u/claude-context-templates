#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Index/IncludeEdge.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace aistudio::core {

// Aggregates `#include` edges across every source file under a project
// root — docs/ROADMAP.md Phase 3 "Code Intelligence" > "Include Graph",
// and the first building block toward Phase 3 "Dependency" > "Code
// dependency" and "Impact Analysis" (what does this file drag in / what
// breaks if I change it). Built on FileScanner (reuses its ignore rules,
// same as SymbolIndex) + IncludeExtractor's tree-sitter-based per-file
// parsing.
//
// Build() runs in two passes: per-file extraction is cached (Cache<T>,
// keyed by path, versioned on FileMetadata::content_hash — identical
// pattern to SymbolIndex/FileCache) so unchanged files skip re-parsing;
// resolving each raw include_text against the project's file list is
// always redone, since it's cheap string matching (no re-parsing) and
// the file list itself may have changed. Resolution matches an
// include_text against the *shortest* scanned path ending in it — e.g.
// `#include "Core/Index/Symbol.hpp"` resolves to
// `Core/include/Core/Index/Symbol.hpp` — so it works without knowing the
// project's actual compiler include-search-path configuration. System
// headers (`<...>`) and anything with no matching project file are left
// unresolved (IncludeEdge::resolved_path stays empty) rather than
// guessed at.
//
// Thread-safe: every public method locks an internal mutex around its
// access to edges_/known_paths_ (Cache<T> already locks its own state
// internally) -- same reasoning as SymbolIndex (docs/ROADMAP.md "File
// Watcher"): a FileWatcher's background thread may drive UpdateFile()/
// RemoveFile() while another thread reads this index.
class IncludeGraph {
public:
    // `extra_ignore_patterns`: see SymbolIndex::Build's identical parameter.
    Result<void> Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns = {});

    // Re-extracts just `path`'s own #include edges (without re-scanning
    // or re-reading any other file's content -- docs/ROADMAP.md
    // "Incremental Reload"), then re-resolves EVERY edge's resolved_path
    // against the updated known-paths set. That second step is
    // unavoidable here in a way SymbolIndex's UpdateFile() didn't need:
    // resolving one edge depends on the *entire* project's file list
    // (see the class comment above), so adding/renaming `path` can
    // change which OTHER edges resolve, not just this file's own -- e.g.
    // adding "Core/Foo.hpp" can newly resolve some unrelated file's
    // `#include "Foo.hpp"` that was previously unresolved. Re-resolution
    // is cheap string matching (no file I/O or re-parsing), so this
    // stays fast even though it touches every edge, not just `path`'s.
    Result<void> UpdateFile(const std::string& root, const std::string& path);

    // Removes `path` from the known-paths set and every edge whose
    // from_file is `path`, evicts its Cache<T> entry, then re-resolves
    // every remaining edge (removing `path` can also un-resolve edges
    // that pointed at it). No-op (still Ok) if `path` was never known.
    Result<void> RemoveFile(const std::string& path);

    // Files this file directly includes (resolved project files only).
    [[nodiscard]] std::vector<std::string> Includes(const std::string& file_path) const;
    // Files that directly include this file (resolved project files only).
    [[nodiscard]] std::vector<std::string> IncludedBy(const std::string& file_path) const;

    [[nodiscard]] std::vector<IncludeEdge> AllEdges() const;
    [[nodiscard]] std::size_t Size() const;
    // Cache<T>::Stats() locks Cache's own internal mutex, not this
    // class's -- no lock needed here, there's nothing of ours to protect.
    [[nodiscard]] CacheStats Stats() const { return cache_.Stats(); }

private:
    [[nodiscard]] static bool IsSourceFile(const std::string& path);
    // Re-runs #include resolution for every entry in edges_ against the
    // current known_paths_. Caller must already hold mutex_.
    void ReResolveAllEdgesLocked();

    mutable std::mutex mutex_; // guards edges_/known_paths_ only -- cache_ is separately thread-safe
    std::vector<IncludeEdge> edges_;
    // Every scanned project path (not just source files -- an include's
    // resolution TARGET can be any file, same set Build() used to call
    // `all_paths`), kept up to date incrementally by UpdateFile()/
    // RemoveFile() so re-resolution doesn't need a fresh FileScanner::Scan().
    std::vector<std::string> known_paths_;
    Cache<std::vector<IncludeEdge>> cache_;
};

} // namespace aistudio::core
