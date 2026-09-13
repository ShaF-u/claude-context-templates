#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Project/FileCache.hpp"
#include "Core/Project/FileMetadata.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Walks a project directory and produces FileMetadata for every file that
// survives the ignore rules — the entry point for Context Engine's File
// Intelligence (docs/ROADMAP.md Phase 2). Synchronous and uncached by
// itself; FileCache / FileWatcher build on top of it (AGENT.md #14 —
// minimal implementation first, extend later).
class FileScanner {
public:
    struct Options {
        // Glob-lite patterns to skip: '*' matches any run of characters
        // within one path segment. A pattern without '/' is checked
        // against every path segment (like .gitignore's basename
        // matching, e.g. "node_modules" skips it at any depth); one
        // containing '/' is checked against the full relative path.
        std::vector<std::string> ignore_patterns = DefaultIgnorePatterns();
    };

    [[nodiscard]] static std::vector<std::string> DefaultIgnorePatterns();

    // DefaultIgnorePatterns() plus `extra_ignore_patterns` appended (e.g.
    // from a host project's own aistudio.config `scan.extra_ignore_patterns`
    // -- see main.cpp) -- the shape every Index::Build(root,
    // extra_ignore_patterns) uses to construct its own internal FileScanner,
    // so a host project can exclude paths (a vendored tool checked out
    // under a project-specific folder name, say) without that name being
    // hardcoded into this template's own generic defaults above.
    [[nodiscard]] static Options MakeOptions(const std::vector<std::string>& extra_ignore_patterns);

    explicit FileScanner(Options options = {}) : options_(std::move(options)) {}

    // `root` is walked recursively; returned paths are relative to root
    // using '/' separators regardless of platform.
    //
    // `content_cache`, when set, is populated with each file's content as
    // it's read for hashing (no extra read) -- a way for callers who scan
    // the same tree more than once per request (ContextRetriever) to
    // share content instead of every consumer reading disk again.
    [[nodiscard]] Result<std::vector<FileMetadata>> Scan(const std::string& root,
                                                          FileCache* content_cache = nullptr) const;

    [[nodiscard]] bool IsIgnored(const std::string& relative_path) const;

private:
    Options options_;
};

// Fast, non-cryptographic 64-bit content hash (FNV-1a). Used for change
// detection in File Cache / Incremental Reload / FileWatcher — never for
// security or content integrity guarantees.
[[nodiscard]] std::string HashContent(const std::string& content);

} // namespace aistudio::core
