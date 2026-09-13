#pragma once

#include "Core/Cache/Cache.hpp"
#include "Core/Cache/CacheStats.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Index/AstNode.hpp"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// Holds one AstNode tree per source file under a project root —
// docs/ROADMAP.md Phase 3 "AST" and Phase 2 "AST Cache". Built on
// FileScanner (reuses its ignore rules, same as SymbolIndex/IncludeGraph/
// CallGraph/InheritanceGraph/ReferenceGraph) + AstExtractor's tree-sitter-
// based per-file parsing, with the same Cache<T>-backed per-file
// extraction (keyed by path, versioned on FileMetadata::content_hash) so
// unchanged files skip re-parsing on rebuild.
//
// Unlike those other indexes, a full AstNode tree can be large (every
// named node in a file, not just its declarations), so this deliberately
// stores one tree per file (Get(path)) rather than flattening everything
// into a single project-wide collection the way SymbolIndex::All() does —
// a caller after one file's structure shouldn't have to pay for every
// other file's tree passing through it too. That per-path map shape also
// makes UpdateFile()/RemoveFile() below simpler than the other indexes'
// own versions: no std::remove_if scan needed, just an upsert/erase by
// key.
//
// Thread-safe: every public method locks an internal mutex around its
// access to trees_ (Cache<T> already locks its own state internally) --
// same reasoning as SymbolIndex (docs/ROADMAP.md "File Watcher").
class AstIndex {
public:
    // `extra_ignore_patterns`: see SymbolIndex::Build's identical parameter.
    Result<void> Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns = {});

    // Re-extracts just `path`'s own AST and upserts it into this index,
    // without re-scanning the rest of the project -- docs/ROADMAP.md
    // "Incremental Reload". Like CallGraph/InheritanceGraph/ReferenceGraph's
    // own UpdateFile(), no cross-file dependency exists here (each tree
    // is entirely self-contained), so this needs no re-resolution step.
    // No-op (Ok) if `path` isn't a recognized source file extension.
    Result<void> UpdateFile(const std::string& root, const std::string& path);

    // Removes `path`'s tree (if any) and evicts its Cache<T> entry.
    // No-op (still Ok) if `path` had no indexed tree.
    Result<void> RemoveFile(const std::string& path);

    [[nodiscard]] std::optional<AstNode> Get(const std::string& file_path) const;
    [[nodiscard]] std::size_t Size() const;
    // Cache<T>::Stats() locks Cache's own internal mutex, not this
    // class's -- no lock needed here, there's nothing of ours to protect.
    [[nodiscard]] CacheStats Stats() const { return cache_.Stats(); }

private:
    [[nodiscard]] static bool IsSourceFile(const std::string& path);

    mutable std::mutex mutex_; // guards trees_ only -- cache_ is separately thread-safe
    std::unordered_map<std::string, AstNode> trees_;
    Cache<AstNode> cache_;
};

} // namespace aistudio::core
