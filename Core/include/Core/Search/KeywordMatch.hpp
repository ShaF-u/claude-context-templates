#pragma once

#include <string>

namespace aistudio::core {

// One line of source text containing a Keyword Search hit —
// docs/ROADMAP.md Phase 3 "Semantic Search" > "Keyword Search",
// docs/MASTER_SPEC.md #13. Line granularity rather than a byte range —
// enough to locate and preview a hit; a caller wanting fuller context
// around it re-reads the file directly (e.g. via FileContextSource).
struct KeywordMatch {
    std::string file_path; // relative, matches FileMetadata::path
    int line = 0;          // 1-based
    std::string text;      // the matching line, trimmed of leading/trailing whitespace
    // Relevance within this result set — see KeywordSearch::Search.
    // Only meaningful for ordering matches against each other, not as an
    // absolute quality measure.
    int score = 0;
};

} // namespace aistudio::core
