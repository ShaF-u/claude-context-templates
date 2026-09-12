#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Project/FileCache.hpp"
#include "Core/Project/FileMetadata.hpp"
#include "Core/Search/KeywordMatch.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aistudio::core {

// Plain case-insensitive substring search across every scanned file's
// text content, line by line — docs/ROADMAP.md Phase 3 "Semantic
// Search" > "Keyword Search", the most basic of the search modes
// docs/MASTER_SPEC.md #13 lists (Semantic/Embedding/Hybrid still need an
// embedding provider — Phase 4 "LLM Integration", not yet implemented —
// so they're deferred rather than attempted without one).
//
// Unlike the Index classes (SymbolIndex, ReferenceGraph, AstIndex, ...),
// this does a fresh scan + read on every Search() call rather than
// building and holding a persistent index — a free-text query has no
// stable per-query cache key the way a content-hash-keyed Symbol
// extraction does.
class KeywordSearch {
public:
    // Ranks hits by relevance (occurrence count on the line, with a
    // bonus for a whole-word match over a mid-word substring one) so the
    // most useful lines sort first, then truncates to `max_results` —
    // docs/ROADMAP.md Phase 3 "Search ranking" for this search mode.
    // `query` empty returns an empty result (Ok, not an error) rather
    // than matching every line. Fails only if `root` itself can't be
    // scanned (e.g. missing), matching FileScanner::Scan / the other
    // Index classes' Build().
    [[nodiscard]] Result<std::vector<KeywordMatch>> Search(const std::string& root, const std::string& query,
                                                             std::size_t max_results = 200) const;

    // Same ranking/truncation, but over an already-scanned file list
    // instead of scanning `root` again -- for a caller (ContextRetriever)
    // that already has one. `content_cache`, when set, is checked before
    // falling back to reading `root`/path from disk -- a cache already
    // populated by the same scan (FileScanner::Scan's own content_cache
    // parameter) means no file is read twice.
    [[nodiscard]] Result<std::vector<KeywordMatch>> Search(const std::vector<FileMetadata>& files,
                                                             const std::string& root, const std::string& query,
                                                             std::size_t max_results = 200,
                                                             FileCache* content_cache = nullptr) const;
};

} // namespace aistudio::core
