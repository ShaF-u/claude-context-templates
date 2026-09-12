#include "Core/Context/ContextRestorer.hpp"

#include "Core/Context/DependencyContextSource.hpp"
#include "Core/Context/FileContextSource.hpp"
#include "Core/Context/SymbolContextSource.hpp"
#include "Core/Project/FileScanner.hpp" // HashContent
#include "Core/Util/Utf8.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>

namespace aistudio::core {

namespace {

namespace fs = std::filesystem;

// Reads one path directly rather than reusing FileScanner::Scan() (a full
// directory walk) — Restore() only ever needs a handful of specific ids,
// not every file under root. Mirrors FileScanner::Scan()'s own read
// pattern (ifstream + HashContent) so a restored item's content_hash is
// comparable with one FileScanner itself would have produced.
struct ReadFile {
    FileMetadata metadata;
    std::string content;
};

std::optional<ReadFile> TryReadFile(const std::string& root, const std::string& relative_path) {
    const fs::path absolute = Utf8ToPath(root) / Utf8ToPath(relative_path);
    std::ifstream file(absolute, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt; // not a file, or deleted since
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    FileMetadata metadata;
    metadata.path = relative_path;
    metadata.size = static_cast<std::uint64_t>(content.size());
    std::error_code time_ec;
    const auto write_time = fs::last_write_time(absolute, time_ec);
    metadata.last_write_time = time_ec ? 0 : write_time.time_since_epoch().count();
    metadata.content_hash = HashContent(content);

    return ReadFile{std::move(metadata), std::move(content)};
}

bool PassesFirewall(const Sandbox* firewall, const std::string& path) {
    return firewall == nullptr || firewall->IsAllowed(path);
}

// File-kind restoration (also the fallback strategy for a legacy
// snapshot with no recorded source kinds, see ContextRestorer.hpp).
void RestoreFile(const ContextRestorer::Options& options, const std::string& id, std::vector<ContextItem>& out) {
    if (!PassesFirewall(options.firewall, id)) {
        return;
    }
    auto read = TryReadFile(options.project_root, id);
    if (!read.has_value()) {
        return;
    }
    out.push_back(MakeFileContextItem(read->metadata, std::move(read->content)));
}

// Symbol-kind restoration: id is "path:name" (SymbolContextSource's
// convention). `path` never contains ':' itself (SymbolIndex only
// indexes recognized source extensions, so it always ends in one, e.g.
// ".cpp"), so splitting on the FIRST ':' is exact even though `name` can
// itself contain further ':' (a qualified member name like "Foo::x") or
// "->" (an operator overload like "operator->").
void RestoreSymbol(const ContextRestorer::Options& options, const std::string& id, std::vector<ContextItem>& out) {
    const auto separator = id.find(':');
    if (separator == std::string::npos) {
        return; // malformed -- not actually "path:name"
    }
    const std::string path = id.substr(0, separator);
    const std::string name = id.substr(separator + 1);
    if (!PassesFirewall(options.firewall, path)) {
        return;
    }
    if (options.symbol_index == nullptr) {
        return;
    }
    for (const auto& symbol : options.symbol_index->FindByName(name)) {
        if (symbol.file_path == path) {
            out.push_back(MakeSymbolContextItem(symbol));
            return; // (path, name) is unique within SymbolIndex -- first match is the only one
        }
    }
    // No current symbol matches -- removed/renamed since the snapshot,
    // treated the same as a deleted file: silently skipped.
}

// Dependency-kind restoration: id is "pathA->pathB" (DependencyContextSource's
// convention), always meaning "pathA includes pathB" regardless of which
// retrieval direction (Includes/IncludedBy) originally produced it -- see
// DependencyContextSource.cpp's MakeItem(). Neither path can contain '>'
// (reserved in a Windows path), so the first "->" is the exact separator.
void RestoreDependency(const ContextRestorer::Options& options, const std::string& id, std::vector<ContextItem>& out) {
    const auto separator = id.find("->");
    if (separator == std::string::npos) {
        return; // malformed -- not actually "pathA->pathB"
    }
    const std::string path_a = id.substr(0, separator);
    const std::string path_b = id.substr(separator + 2);
    if (!PassesFirewall(options.firewall, path_a) || !PassesFirewall(options.firewall, path_b)) {
        return;
    }
    if (options.include_graph == nullptr) {
        return;
    }
    // Reuses DependencyContextSource itself (AGENT.md #1 "既存機能を再利用
    // できないか確認する") rather than re-deriving its content-string
    // format here -- also doubles as the "does this edge still exist"
    // check: if `include_graph` no longer has path_a -> path_b, no
    // returned item's id will match and nothing is appended.
    for (auto& item : MakeDependencyContextItems(*options.include_graph, path_a, DependencyDirection::Includes, 40)) {
        if (item.id == id) {
            out.push_back(std::move(item));
            return;
        }
    }
}

// Restored keyword items don't have their original KeywordMatch::score
// (ContextSnapshot never persisted it, only the id) to feed back through
// ContextRetriever.cpp's KeywordPriority(), so this uses a fixed value in
// the same 15-35 band that function clamps to -- keyword retrieval was
// already documented there as "the weakest, broadest signal", so a
// restored keyword item defaulting toward the low end of that band stays
// consistent with that framing.
constexpr int kRestoredKeywordPriority = 20;
constexpr std::string_view kKeywordIdPrefix = "keyword:";

// Custom-kind restoration, but ONLY for ids matching ContextRetriever's
// "keyword:path:line" convention -- other Custom ids (Backend/Plugin
// -provided, e.g. ProjectRulesBackend's "rules/project") have no source
// this class can re-read and are left alone. Unlike File/Symbol/
// Dependency, this needs no persistent index: KeywordSearch itself does a
// fresh scan-and-read per query rather than building one (see its own
// class comment), so restoring one match is just re-reading that file's
// one line directly, the same file-open primitive File-kind restoration
// already uses.
void RestoreKeywordShapedCustom(const ContextRestorer::Options& options, const std::string& id,
                                 std::vector<ContextItem>& out) {
    if (id.rfind(kKeywordIdPrefix, 0) != 0) {
        return; // an opaque Custom id from some other source -- not restorable here
    }
    const std::string rest = id.substr(kKeywordIdPrefix.size());
    const auto separator = rest.rfind(':'); // last ':' -- `path` may itself contain none, but never digits-only
    if (separator == std::string::npos) {
        return; // malformed
    }
    const std::string path = rest.substr(0, separator);
    const std::string line_text = rest.substr(separator + 1);
    int line_number = 0;
    const auto parse_result = std::from_chars(line_text.data(), line_text.data() + line_text.size(), line_number);
    if (parse_result.ec != std::errc{} || parse_result.ptr != line_text.data() + line_text.size() || line_number < 1) {
        return; // malformed -- the trailing segment isn't a plain positive line number
    }
    if (!PassesFirewall(options.firewall, path)) {
        return;
    }

    const fs::path absolute = Utf8ToPath(options.project_root) / Utf8ToPath(path);
    std::ifstream file(absolute, std::ios::binary);
    if (!file.is_open()) {
        return; // deleted/renamed since the snapshot
    }
    std::string line;
    for (int current = 0; current < line_number; ++current) {
        if (!std::getline(file, line)) {
            return; // file has fewer lines now than it did at snapshot time
        }
    }
    // Trim the same way KeywordSearch::Search()'s TrimLine() does, so a
    // restored item's content matches what a fresh keyword search would
    // produce for this same line today.
    std::size_t end = line.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(line[end - 1])) != 0) {
        --end;
    }
    std::size_t start = 0;
    while (start < end && std::isspace(static_cast<unsigned char>(line[start])) != 0) {
        ++start;
    }
    const std::string trimmed = line.substr(start, end - start);

    ContextItem item;
    item.id = id;
    item.source = ContextSourceKind::Custom;
    item.compression = CompressionLevel::Reference;
    item.priority = kRestoredKeywordPriority;
    item.content = path + ":" + std::to_string(line_number) + ": " + trimmed;
    item.estimated_tokens = EstimateTokens(item.content);
    item.depends_on = {path};
    out.push_back(std::move(item));
}

} // namespace

std::vector<ContextItem> ContextRestorer::Restore(const ContextSnapshot& snapshot) const {
    std::vector<ContextItem> items;
    items.reserve(snapshot.included_item_ids.size());

    // A pre-migration snapshot has no (or a misaligned) kinds vector --
    // fall back to the original file-shape-only guess for every id in
    // that case rather than reading kinds[i] out of bounds/misaligned
    // (see this class's own header comment).
    const bool has_source_kinds = snapshot.included_item_source_kinds.size() == snapshot.included_item_ids.size();

    for (std::size_t i = 0; i < snapshot.included_item_ids.size(); ++i) {
        const auto& id = snapshot.included_item_ids[i];
        if (!has_source_kinds) {
            RestoreFile(options_, id, items);
            continue;
        }
        switch (snapshot.included_item_source_kinds[i]) {
            case ContextSourceKind::File:
                RestoreFile(options_, id, items);
                break;
            case ContextSourceKind::Symbol:
                RestoreSymbol(options_, id, items);
                break;
            case ContextSourceKind::Dependency:
                RestoreDependency(options_, id, items);
                break;
            case ContextSourceKind::Custom:
                RestoreKeywordShapedCustom(options_, id, items);
                break;
            case ContextSourceKind::GitDiff:
                // No restoration source yet (Phase 6 Git Backend content
                // isn't re-readable through an index the way File/
                // Symbol/Dependency are) -- skipped, same as any other
                // unresolvable id.
                break;
        }
    }

    return items;
}

} // namespace aistudio::core
