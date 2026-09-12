#include "Core/Context/FileProviderBackend.hpp"

#include "Core/Backend/BackendFactoryRegistry.hpp"
#include "Core/Context/FileContextSource.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Util/TextRange.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aistudio::core {

namespace {

// Files larger than this are skipped entirely (not read, not scored) --
// a heuristic content match on a multi-megabyte log or generated file
// isn't worth pulling fully into memory/context.
constexpr std::uint64_t kMaxFileSizeBytes = 262144; // 256 KiB

std::string ToLower(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Non-overlapping occurrence count in one already-lower-cased line.
int CountOccurrences(const std::string& lower_line, const std::string& lower_needle) {
    int count = 0;
    std::size_t pos = 0;
    while (true) {
        pos = lower_line.find(lower_needle, pos);
        if (pos == std::string::npos) {
            break;
        }
        ++count;
        pos += lower_needle.size();
    }
    return count;
}

struct ContentMatch {
    int occurrences = 0;
    std::vector<LineRange> ranges; // one context window per matching line
};

// Line-scoped, like KeywordSearch: an intent containing a newline no
// longer matches across a line break (it did when this scanned the whole
// blob at once).
ContentMatch FindMatches(const std::string& content, const std::string& lower_needle, int context_lines) {
    ContentMatch result;
    std::istringstream stream(content);
    std::string line;
    int line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        const int occurrences = CountOccurrences(ToLower(line), lower_needle);
        if (occurrences == 0) {
            continue;
        }
        result.occurrences += occurrences;
        result.ranges.push_back(LineRange{std::max(1, line_number - context_lines), line_number + context_lines});
    }
    return result;
}

std::optional<std::size_t> ParseSizeT(const std::string& text) {
    std::size_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

} // namespace

Result<void> FileProviderBackend::Start() {
    file_changed_subscription_id_ =
        EventBus::Instance().Subscribe("FileChanged", [this](const std::any&) { scan_cache_.Invalidate("scan"); });
    return Result<void>::Ok();
}

Result<void> FileProviderBackend::Stop() {
    EventBus::Instance().Unsubscribe("FileChanged", file_changed_subscription_id_);
    return Result<void>::Ok();
}

Result<void> FileProviderBackend::Configure(const Config& config) {
    root_ = config.GetOr("backend.core.file_provider.root", config.GetOr("project.root", "."));
    if (const auto max_results_str = config.Get("backend.core.file_provider.max_results")) {
        if (const auto parsed = ParseSizeT(*max_results_str)) {
            max_results_ = *parsed;
        }
    }
    if (const auto context_lines_str = config.Get("backend.core.file_provider.context_lines")) {
        if (const auto parsed = ParseSizeT(*context_lines_str)) {
            context_lines_ = *parsed;
        }
    }
    return Result<void>::Ok();
}

CommandResult FileProviderBackend::Dispatch(const Command& command) {
    return CommandResult::Fail(Error{.code = ErrorCode::NotFound,
                                      .message = "core.file_provider does not support command: " + command.name,
                                      .module = "Context.FileProviderBackend"});
}

QueryResult FileProviderBackend::Handle(const Query&) {
    return QueryResult::Fail(Error{.code = ErrorCode::NotFound,
                                    .message = "core.file_provider does not support any queries yet",
                                    .module = "Context.FileProviderBackend"});
}

std::vector<ContextItem> FileProviderBackend::ProvideContext(const std::string& intent) const {
    std::vector<ContextItem> items;
    if (root_.empty() || intent.empty()) {
        return items;
    }

    std::vector<FileMetadata> scanned_files;
    if (auto cached_scan = scan_cache_.Get("scan")) {
        scanned_files = std::move(*cached_scan);
    } else {
        const FileScanner scanner;
        const auto scan_result = scanner.Scan(root_);
        if (!scan_result) {
            return items;
        }
        scanned_files = scan_result.Value();
        scan_cache_.Put("scan", scanned_files);
    }

    const std::string lower_intent = ToLower(intent);
    const std::filesystem::path root_path = Utf8ToPath(root_);

    struct Match {
        int score;
        const FileMetadata* metadata;
        std::string payload;    // excerpt, or the whole file when the windows cover it
        bool whole_file = true; // drives CompressionLevel below
    };
    std::vector<Match> matches;

    for (const auto& metadata : scanned_files) {
        if (metadata.size > kMaxFileSizeBytes) {
            continue;
        }

        std::string content;
        if (auto cached = cache_.Get(metadata)) {
            content = std::move(*cached);
        } else {
            std::ifstream file(root_path / Utf8ToPath(metadata.path), std::ios::binary);
            if (!file.is_open()) {
                continue;
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();
            content = buffer.str();
            cache_.Put(metadata, content);
        }

        // FileScanner has no text/binary distinction -- an image, a
        // .jar, or any other binary asset a real Unity/Unreal project is
        // full of would otherwise become ContextItem::content that
        // crashes the whole response at JSON serialization (invalid
        // UTF-8), not just get skipped for this one file.
        if (!IsValidUtf8(content)) {
            continue;
        }

        const auto found = FindMatches(content, lower_intent, static_cast<int>(context_lines_));
        if (found.occurrences == 0) {
            continue;
        }

        // Deliberately a different band from Symbol (35-85), Keyword
        // (15-35) and the built-in File retrieval step's raw path score
        // (0-60) -- see class comment for why this Backend's matches are
        // additive to, not competing with, those.
        const int score = std::clamp(20 + found.occurrences * 8, 20, 65);

        // Whole file only when the windows already cover it (the excerpt
        // header would be pure overhead over identical bytes).
        const int total_lines = CountLines(content);
        const auto merged = MergeLineRanges(found.ranges);
        const bool whole_file =
            merged.size() == 1 && merged.front().start <= 1 && merged.front().end >= total_lines;

        std::string payload = whole_file ? std::move(content) : BuildExcerpt(metadata.path, content, merged);
        matches.push_back(Match{score, &metadata, std::move(payload), whole_file});
    }

    std::stable_sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.score > b.score; });
    if (matches.size() > max_results_) {
        matches.resize(max_results_);
    }

    items.reserve(matches.size());
    for (auto& match : matches) {
        auto item = MakeFileContextItem(*match.metadata, std::move(match.payload), match.score);
        if (!match.whole_file) {
            item.compression = CompressionLevel::Summary;
        }
        items.push_back(std::move(item));
    }
    return items;
}

AISTUDIO_REGISTER_BACKEND(FileProviderBackend);

} // namespace aistudio::core
