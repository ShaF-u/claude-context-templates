#include "Core/Util/TextRange.hpp"

#include <algorithm>

namespace aistudio::core {

namespace {

std::vector<std::string> SplitLines(const std::string& content) {
    std::vector<std::string> lines;
    if (content.empty()) {
        return lines;
    }

    std::string current;
    for (const char c : content) {
        if (c == '\n') {
            if (!current.empty() && current.back() == '\r') {
                current.pop_back();
            }
            lines.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) {
        if (current.back() == '\r') {
            current.pop_back();
        }
        lines.push_back(std::move(current));
    }
    return lines;
}

std::string JoinRange(const std::vector<std::string>& lines, int start_line, int end_line) {
    std::string result;
    for (int line = start_line; line <= end_line; ++line) {
        if (!result.empty()) {
            result += '\n';
        }
        result += lines[static_cast<std::size_t>(line - 1)];
    }
    return result;
}

} // namespace

int CountLines(const std::string& content) {
    return static_cast<int>(SplitLines(content).size());
}

std::string ExtractLines(const std::string& content, int start_line, int end_line) {
    const auto lines = SplitLines(content);
    if (lines.empty() || start_line > end_line) {
        return {};
    }

    const int total = static_cast<int>(lines.size());
    const int first = std::max(1, start_line);
    const int last = std::min(total, end_line);
    if (first > last) {
        return {};
    }
    return JoinRange(lines, first, last);
}

std::vector<LineRange> MergeLineRanges(std::vector<LineRange> ranges) {
    std::vector<LineRange> valid;
    valid.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (range.start > 0 && range.end >= range.start) {
            valid.push_back(range);
        }
    }

    std::sort(valid.begin(), valid.end(),
               [](const LineRange& a, const LineRange& b) { return a.start < b.start; });

    std::vector<LineRange> merged;
    for (const auto& range : valid) {
        // -1 so touching blocks merge too, not just overlapping ones.
        if (!merged.empty() && range.start - 1 <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, range.end);
            continue;
        }
        merged.push_back(range);
    }
    return merged;
}

std::string BuildExcerpt(const std::string& label, const std::string& content, std::vector<LineRange> ranges) {
    const auto lines = SplitLines(content);
    if (lines.empty()) {
        return {};
    }
    const int total = static_cast<int>(lines.size());

    // Clamp before merging: two windows that both run off the end are the
    // same block once clamped.
    for (auto& range : ranges) {
        range.start = std::max(1, range.start);
        range.end = std::min(total, range.end);
    }

    std::string excerpt;
    for (const auto& range : MergeLineRanges(std::move(ranges))) {
        if (!excerpt.empty()) {
            excerpt += '\n';
        }
        excerpt += "@@ " + label + ":" + std::to_string(range.start) + "-" + std::to_string(range.end) + " @@\n";
        excerpt += JoinRange(lines, range.start, range.end);
    }
    return excerpt;
}

} // namespace aistudio::core
