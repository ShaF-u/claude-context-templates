#pragma once

#include <string>
#include <vector>

namespace aistudio::core {

// 1-based, inclusive — the numbering Symbol::line / KeywordMatch::line /
// AstNode::start_line already use.
struct LineRange {
    int start = 0;
    int end = 0;
};

// Line-range utilities for the granularity ladder (docs/MASTER_SPEC.md
// #99): McpServer's context_fetch and FileProviderBackend's excerpts.
// Lines split on '\n', trailing '\r' dropped — an excerpt to be read, not
// a byte-exact slice.

// A trailing newline adds no line: "a\nb\n" and "a\nb" are both 2.
[[nodiscard]] int CountLines(const std::string& content);

// Clamped to what exists; empty if the range is outside `content` or
// inverted. No trailing newline.
[[nodiscard]] std::string ExtractLines(const std::string& content, int start_line, int end_line);

// Sorts and merges overlapping or adjacent ranges ([1,5] + [6,9] =
// [1,9]); drops non-positive or inverted ones.
[[nodiscard]] std::vector<LineRange> MergeLineRanges(std::vector<LineRange> ranges);

// Each block preceded by "@@ <label>:<start>-<end> @@", so a reader can
// see which lines these are and that material between blocks is missing.
// Ranges are clamped and merged first.
[[nodiscard]] std::string BuildExcerpt(const std::string& label, const std::string& content,
                                        std::vector<LineRange> ranges);

} // namespace aistudio::core
