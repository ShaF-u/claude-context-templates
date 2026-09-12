#include "Core/Analysis/ImpactAnalyzer.hpp"

#include "Core/Util/Identifier.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <unordered_set>

namespace aistudio::core {

namespace {

struct LineRange {
    int start = 0;
    int end = 0; // inclusive
};

struct FileDiff {
    std::string file_path;
    std::vector<LineRange> ranges;
};

// Parses one integer starting at `pos` in `text`, stopping at the first
// non-digit. Returns nullopt (rather than throwing) on malformed input,
// matching this codebase's std::from_chars-based, exception-free parsing
// style (see CapabilityVersion::Parse).
std::optional<int> ParseIntAt(const std::string& text, std::size_t pos, std::size_t& end_pos) {
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data() + pos, text.data() + text.size(), value);
    if (ec != std::errc() || ptr == text.data() + pos) {
        return std::nullopt;
    }
    end_pos = static_cast<std::size_t>(ptr - text.data());
    return value;
}

// Parses one "@@ -a,b +c,d @@" hunk header and returns just the new-side
// start line c (",d" is irrelevant here -- ParseUnifiedDiff walks the
// hunk body itself to find exactly which new-side lines changed, rather
// than trusting the header's line count).
std::optional<int> ParseHunkNewStart(const std::string& line) {
    const std::size_t plus = line.find('+');
    if (plus == std::string::npos) {
        return std::nullopt;
    }
    std::size_t unused = 0;
    return ParseIntAt(line, plus + 1, unused);
}

// Strips a git-style "a/" or "b/" prefix from a diff header path, e.g.
// "+++ b/Core/main.cpp" -> "Core/main.cpp". Paths this codebase deals
// with never legitimately start with a bare "a/"/"b/" segment (relative
// to project root), so this is unambiguous.
std::string StripDiffPrefix(std::string path) {
    if (path.size() > 2 && (path.rfind("a/", 0) == 0 || path.rfind("b/", 0) == 0)) {
        return path.substr(2);
    }
    return path;
}

// Walks `git diff`'s raw unified-diff text and, per file, records exactly
// which NEW-side line numbers changed -- not the coarser "whole hunk
// span including context lines" a header alone would give, which for a
// small file (git's default 3-line context) can easily cover an entire
// function that was never touched. Added ('+') lines mark their own new
// line number; removed ('-') lines mark the new-side position they once
// preceded (nothing to increment, since they no longer exist in the new
// file) so a pure deletion still registers as "something changed here."
// Context (' ') lines just advance the new-line cursor.
std::vector<FileDiff> ParseUnifiedDiff(const std::string& diff) {
    std::vector<FileDiff> files;
    FileDiff* current = nullptr;
    bool in_hunk = false;
    int new_line_cursor = 0;

    const auto record_changed_line = [&](int line_number) {
        if (current == nullptr) {
            return;
        }
        if (!current->ranges.empty() && current->ranges.back().end + 1 == line_number) {
            current->ranges.back().end = line_number;
        } else {
            current->ranges.push_back(LineRange{line_number, line_number});
        }
    };

    std::size_t line_start = 0;
    while (line_start <= diff.size()) {
        const std::size_t newline = diff.find('\n', line_start);
        const std::string line =
            diff.substr(line_start, (newline == std::string::npos ? diff.size() : newline) - line_start);

        // `!in_hunk` matters: a hunk BODY line can legitimately start
        // with "+++ " too -- an added content line whose own text starts
        // with "++ " renders, once git prepends its own '+' marker, as
        // "+++ <rest>" (confirmed with a real `git diff`: adding a line
        // literally reading "++ this looks like a header" produces
        // exactly that). Without this guard such a line gets misread as
        // a new file's header mid-hunk, silently misattributing every
        // subsequent line in the CURRENT file's hunk to a bogus
        // "file" entry (a false negative -- the real file's remaining
        // changes go undetected). A genuine "+++ " header only ever
        // appears between files, where a "diff --git a/X b/X" line
        // (matching neither '+'/'-'/' '/'\\') always already reset
        // in_hunk to false first, so this guard costs nothing there.
        if (!in_hunk && line.rfind("+++ ", 0) == 0) {
            const std::string raw_path = line.substr(4);
            if (raw_path == "/dev/null") {
                current = nullptr; // deleted file -- nothing on the new side to correlate
            } else {
                const std::string path = StripDiffPrefix(raw_path);
                // A caller may concatenate more than one diff for the
                // same file (e.g. unstaged + staged changes both
                // touching it) -- merge into the existing entry rather
                // than reporting the file twice.
                const auto existing =
                    std::find_if(files.begin(), files.end(), [&](const FileDiff& f) { return f.file_path == path; });
                if (existing != files.end()) {
                    current = &*existing;
                } else {
                    files.push_back(FileDiff{path, {}});
                    current = &files.back();
                }
            }
        } else if (current != nullptr && line.rfind("@@ ", 0) == 0) {
            if (const auto new_start = ParseHunkNewStart(line)) {
                new_line_cursor = *new_start;
                in_hunk = true;
            } else {
                in_hunk = false;
            }
        } else if (in_hunk && current != nullptr && !line.empty()) {
            if (line[0] == '+') {
                record_changed_line(new_line_cursor);
                ++new_line_cursor;
            } else if (line[0] == '-') {
                record_changed_line(new_line_cursor);
            } else if (line[0] == ' ') {
                ++new_line_cursor;
            } else if (line[0] != '\\') { // "\ No newline at end of file" -- ignore, stay in hunk
                in_hunk = false;
            }
        }

        if (newline == std::string::npos) {
            break;
        }
        line_start = newline + 1;
    }

    return files;
}

bool Overlaps(const LineRange& a, int start, int end) {
    return a.start <= end && start <= a.end;
}

} // namespace

ImpactAnalyzer::ImpactAnalyzer(const IncludeGraph& include_graph, const CallGraph& call_graph,
                                const SymbolIndex& symbol_index)
    : include_graph_(include_graph), call_graph_(call_graph), symbol_index_(symbol_index) {}

std::vector<std::string> ImpactAnalyzer::CollectTransitiveIncluders(const std::vector<std::string>& seeds) const {
    std::vector<std::string> affected_files;

    // Breadth-first over IncludedBy edges: each level is "one more #include
    // hop away from a seed file". `visited` (seeded with every seed so
    // none re-includes itself) makes this safe against #include cycles.
    std::unordered_set<std::string> visited(seeds.begin(), seeds.end());
    std::vector<std::string> frontier = seeds;
    while (!frontier.empty()) {
        std::vector<std::string> next_frontier;
        for (const auto& current : frontier) {
            for (const auto& includer : include_graph_.IncludedBy(current)) {
                if (visited.insert(includer).second) {
                    affected_files.push_back(includer);
                    next_frontier.push_back(includer);
                }
            }
        }
        frontier = std::move(next_frontier);
    }

    return affected_files;
}

ImpactResult ImpactAnalyzer::Analyze(const std::string& file_path) const {
    ImpactResult result;
    result.target_file = file_path;
    result.affected_files = CollectTransitiveIncluders({file_path});

    for (const auto& symbol : symbol_index_.All()) {
        if (symbol.file_path != file_path || symbol.kind != SymbolKind::Function) {
            continue;
        }
        AffectedSymbol affected;
        affected.symbol_name = symbol.name;
        affected.callers = call_graph_.Callers(TrailingIdentifier(symbol.name));
        result.affected_symbols.push_back(std::move(affected));
    }

    return result;
}

ChangeImpactResult ImpactAnalyzer::AnalyzeChanges(const std::string& unified_diff) const {
    ChangeImpactResult result;

    const auto file_diffs = ParseUnifiedDiff(unified_diff);
    for (const auto& file_diff : file_diffs) {
        result.changed_files.push_back(file_diff.file_path);
    }

    for (const auto& file_diff : file_diffs) {
        if (file_diff.ranges.empty()) {
            continue;
        }

        std::vector<Symbol> symbols_in_file;
        for (const auto& symbol : symbol_index_.All()) {
            if (symbol.file_path == file_diff.file_path) {
                symbols_in_file.push_back(symbol);
            }
        }
        std::stable_sort(symbols_in_file.begin(), symbols_in_file.end(),
                          [](const Symbol& a, const Symbol& b) { return a.line < b.line; });

        for (std::size_t i = 0; i < symbols_in_file.size(); ++i) {
            const int span_start = symbols_in_file[i].line;
            const int span_end = (i + 1 < symbols_in_file.size()) ? symbols_in_file[i + 1].line - 1
                                                                    : std::numeric_limits<int>::max();

            const bool touched = std::any_of(file_diff.ranges.begin(), file_diff.ranges.end(),
                                              [&](const LineRange& range) { return Overlaps(range, span_start, span_end); });
            if (!touched) {
                continue;
            }

            ChangedSymbol changed;
            changed.symbol_name = symbols_in_file[i].name;
            changed.kind = symbols_in_file[i].kind;
            changed.file_path = file_diff.file_path;
            if (changed.kind == SymbolKind::Function) {
                changed.callers = call_graph_.Callers(TrailingIdentifier(changed.symbol_name));
            }
            result.changed_symbols.push_back(std::move(changed));
        }
    }

    result.affected_files = CollectTransitiveIncluders(result.changed_files);
    return result;
}

} // namespace aistudio::core
