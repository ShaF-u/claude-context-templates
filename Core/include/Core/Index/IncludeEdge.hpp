#pragma once

#include <string>

namespace aistudio::core {

// One `#include` directive found in a source file — docs/ROADMAP.md
// Phase 3 "Code Intelligence" > "Include Graph". `resolved_path` is only
// set when `include_text` could be matched against another file the
// IncludeGraph scanned (see IncludeGraph::Build); system headers
// (`<...>`) and headers outside the project are left unresolved rather
// than guessed at, so callers can tell "unresolved" from "resolved to X"
// without a sentinel value.
struct IncludeEdge {
    std::string from_file;    // includer, project-relative path (matches FileMetadata::path)
    std::string include_text; // exactly as written, without quotes/angle brackets
    bool is_system = false;   // true for <...>, false for "..."
    int line = 0;             // 1-based
    std::string resolved_path; // project-relative path of the included file, or empty if unresolved
};

} // namespace aistudio::core
