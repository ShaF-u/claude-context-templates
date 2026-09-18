#include "Core/Search/KeywordSearch.hpp"

#include "Core/Project/FileScanner.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace aistudio::core {

namespace {

std::string ToLower(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string TrimLine(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    std::size_t start = 0;
    while (start < end && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }
    return text.substr(start, end - start);
}

bool IsWordChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

// Counts non-overlapping occurrences of `lower_needle` in `lower_line`
// (both already lower-cased by the caller) and reports whether at least
// one of those occurrences is bounded by non-word characters on both
// sides (or the start/end of the line) — a real word, not a substring
// mid-identifier.
int CountOccurrences(const std::string& lower_line, const std::string& lower_needle, bool& has_whole_word) {
    int count = 0;
    has_whole_word = false;
    std::size_t pos = 0;
    while (true) {
        pos = lower_line.find(lower_needle, pos);
        if (pos == std::string::npos) {
            break;
        }
        ++count;
        const bool left_boundary = pos == 0 || !IsWordChar(lower_line[pos - 1]);
        const std::size_t match_end = pos + lower_needle.size();
        const bool right_boundary = match_end >= lower_line.size() || !IsWordChar(lower_line[match_end]);
        if (left_boundary && right_boundary) {
            has_whole_word = true;
        }
        pos = match_end;
    }
    return count;
}

// One pass over `stream`'s lines for every query at once: the line is
// lower-cased (and, on a hit, trimmed + UTF-8-checked) once, not once
// per query. matches_per_query[i] receives query i's hits. An empty
// query never matches (CountOccurrences would otherwise loop forever on
// an empty needle -- every caller filters those out before this).
void ScanLinesForMatches(const std::string& file_path, std::istream& stream,
                          const std::vector<std::string>& lower_queries,
                          std::vector<std::vector<KeywordMatch>>& matches_per_query) {
    std::string line;
    int line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;

        const std::string lower_line = ToLower(line);
        // Computed lazily on the first query that hits this line, then
        // shared by every later one.
        std::optional<std::string> valid_text;
        bool text_rejected = false;

        for (std::size_t i = 0; i < lower_queries.size(); ++i) {
            if (lower_queries[i].empty()) {
                continue;
            }
            bool has_whole_word = false;
            const int occurrences = CountOccurrences(lower_line, lower_queries[i], has_whole_word);
            if (occurrences == 0) {
                continue;
            }

            if (text_rejected) {
                continue;
            }
            if (!valid_text.has_value()) {
                // FileScanner has no text/binary distinction -- a "line"
                // (0x0A-delimited) read out of a binary file can contain
                // arbitrary bytes, which would crash JSON serialization
                // downstream (invalid UTF-8) rather than just skip this
                // one match.
                std::string text = TrimLine(line);
                if (!IsValidUtf8(text)) {
                    text_rejected = true;
                    continue;
                }
                valid_text = std::move(text);
            }

            KeywordMatch match;
            match.file_path = file_path;
            match.line = line_number;
            match.text = *valid_text;
            match.score = occurrences * 10 + (has_whole_word ? 5 : 0);
            matches_per_query[i].push_back(std::move(match));
        }
    }
}

} // namespace

Result<std::vector<KeywordMatch>> KeywordSearch::Search(const std::string& root, const std::string& query,
                                                          std::size_t max_results) const {
    std::vector<KeywordMatch> matches;
    if (query.empty()) {
        return Result<std::vector<KeywordMatch>>::Ok(std::move(matches));
    }

    const FileScanner scanner;
    const auto scan_result = scanner.Scan(root);
    if (!scan_result) {
        return Result<std::vector<KeywordMatch>>::Fail(scan_result.Err());
    }

    return Search(scan_result.Value(), root, query, max_results, nullptr);
}

Result<std::vector<KeywordMatch>> KeywordSearch::Search(const std::vector<FileMetadata>& files,
                                                          const std::string& root, const std::string& query,
                                                          std::size_t max_results, FileCache* content_cache) const {
    auto result = Search(files, root, std::vector<std::string>{query}, max_results, content_cache);
    if (!result) {
        return Result<std::vector<KeywordMatch>>::Fail(result.Err());
    }
    return Result<std::vector<KeywordMatch>>::Ok(std::move(result.Value().front()));
}

Result<std::vector<std::vector<KeywordMatch>>> KeywordSearch::Search(const std::vector<FileMetadata>& files,
                                                                       const std::string& root,
                                                                       const std::vector<std::string>& queries,
                                                                       std::size_t max_results,
                                                                       FileCache* content_cache) const {
    std::vector<std::vector<KeywordMatch>> matches_per_query(queries.size());

    std::vector<std::string> lower_queries;
    lower_queries.reserve(queries.size());
    bool any_query = false;
    for (const auto& query : queries) {
        lower_queries.push_back(ToLower(query));
        any_query = any_query || !query.empty();
    }
    if (!any_query) {
        return Result<std::vector<std::vector<KeywordMatch>>>::Ok(std::move(matches_per_query));
    }

    const std::filesystem::path root_path = Utf8ToPath(root);

    for (const auto& metadata : files) {
        if (content_cache != nullptr) {
            if (const auto cached = content_cache->Get(metadata)) {
                std::istringstream stream(*cached);
                ScanLinesForMatches(metadata.path, stream, lower_queries, matches_per_query);
                continue;
            }
        }

        std::ifstream file(root_path / Utf8ToPath(metadata.path), std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        ScanLinesForMatches(metadata.path, file, lower_queries, matches_per_query);
    }

    for (auto& matches : matches_per_query) {
        std::stable_sort(matches.begin(), matches.end(),
                          [](const KeywordMatch& a, const KeywordMatch& b) { return a.score > b.score; });
        if (matches.size() > max_results) {
            matches.resize(max_results);
        }
    }

    return Result<std::vector<std::vector<KeywordMatch>>>::Ok(std::move(matches_per_query));
}

} // namespace aistudio::core
