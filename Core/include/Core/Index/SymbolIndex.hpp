#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Index/Symbol.hpp"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace aistudio::core {

// Aggregates Symbols across every source file under a project root —
// docs/ROADMAP.md Phase 3 "Symbol Index". Built on FileScanner (reuses
// its ignore rules) + SymbolExtractor's tree-sitter-based per-file parsing.
// Rebuilding is a full re-scan of the file list, but per-file extraction
// is cached (Cache<T>, keyed by path, versioned on
// FileMetadata::content_hash — same pattern as FileCache) so a rebuild
// after only a few files changed re-parses just those files instead of
// the whole project (docs/ROADMAP.md Phase 2 "Context Cache" — Cache<T>'s
// own doc comment names a Symbol Cache as one of its intended
// consumers). Incremental updates driven by FileWatcher (as opposed to a
// full Build() re-scan) are UpdateFile()/RemoveFile() below, though
// nothing wires an actual FileWatcher to call them yet.
//
// Thread-safe: every public method locks an internal mutex around its
// access to symbols_ (Cache<T> already locks its own state internally).
// This matters once something drives UpdateFile()/RemoveFile() from a
// FileWatcher's background thread (docs/ROADMAP.md "File Watcher") while
// e.g. ApiServer reads this same index from a request-handling thread —
// without it, that combination would be a data race.
class SymbolIndex {
public:
    // `extra_ignore_patterns` is appended to FileScanner's own defaults
    // (see FileScanner::MakeOptions) -- lets a host project exclude extra
    // paths (e.g. this tool's own checkout, see main.cpp's
    // scan.extra_ignore_patterns config) without hardcoding them here.
    Result<void> Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns = {});

    // Re-extracts just `path`'s symbols and replaces them in this index,
    // without re-scanning the rest of the project via FileScanner --
    // docs/ROADMAP.md Phase 2 "Incremental Reload". Intended to be driven
    // by a FileWatcher Created/Modified FileChangeEvent, but takes a plain
    // path rather than depending on FileWatcher's own type (Core/Project),
    // keeping this module's existing dependency direction unchanged (it
    // already reaches into Core/Project for FileScanner/HashContent, but
    // nothing reaches back). No-op (Ok) if `path` isn't a recognized
    // source file extension, matching Build()'s own silent skip.
    Result<void> UpdateFile(const std::string& root, const std::string& path);

    // Removes every symbol previously indexed from `path` (a FileWatcher
    // Deleted event) and evicts its Cache<T> entry. No-op (still Ok) if
    // `path` had no indexed symbols.
    Result<void> RemoveFile(const std::string& path);

    [[nodiscard]] std::vector<Symbol> FindByName(const std::string& name) const;
    [[nodiscard]] std::vector<Symbol> All() const;
    [[nodiscard]] std::size_t Size() const;
    // Cache<T>::Stats() locks Cache's own internal mutex, not this
    // class's -- no lock needed here, there's nothing of ours to protect.
    [[nodiscard]] CacheStats Stats() const { return cache_.Stats(); }

private:
    [[nodiscard]] static bool IsSourceFile(const std::string& path);

    mutable std::mutex mutex_; // guards symbols_ only -- cache_ is separately thread-safe
    std::vector<Symbol> symbols_;
    Cache<std::vector<Symbol>> cache_;
};

} // namespace aistudio::core
