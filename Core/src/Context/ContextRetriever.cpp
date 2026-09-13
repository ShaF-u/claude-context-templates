#include "Core/Context/ContextRetriever.hpp"

#include "Core/Context/DependencyContextSource.hpp"
#include "Core/Context/FileContextSource.hpp"
#include "Core/Context/SymbolContextSource.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Search/KeywordSearch.hpp"
#include "Core/Search/SymbolSearch.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace aistudio::core {

namespace {

std::string ToLower(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Splits a free-text intent into the identifier-like words Symbol/File/
// Keyword retrieval below actually match against. Without this, each of
// those three treats the ENTIRE intent as one literal substring to look
// for (SymbolSearch::ScoreSymbol, FilePathScore, KeywordSearch's
// per-line search) -- a condition a real "player attack logic"-shaped
// intent (this class's own documented example, see the class comment)
// essentially never satisfies, since no symbol/file/line is named
// literally "player attack logic". Tokenizing lets each word be tried
// against every source independently, matching this class's own stated
// intent of accepting free text rather than an exact name.
//
// A run of non-word bytes (spaces, punctuation, or a multi-byte UTF-8
// character's continuation bytes -- all >= 0x80, so none of them count
// as isalnum in the "C" locale) is a delimiter. That incidentally pulls
// an embedded identifier like "SpriteRenderSystem" out of an all-
// Japanese sentence with no spaces at all, which matters in a codebase
// (see this project's own CLAUDE.md) whose comments mix English
// identifiers into Japanese prose with no separator between them.
// Tokens shorter than 2 characters are dropped -- a single letter/digit
// matches too broadly to be a useful signal in any of the three sources
// below.
std::vector<std::string> TokenizeIntent(const std::string& intent) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (current.size() >= 2) {
            tokens.push_back(current);
        }
        current.clear();
    };
    for (const unsigned char c : intent) {
        if (std::isalnum(c) != 0 || c == '_') {
            current.push_back(static_cast<char>(c));
        } else {
            flush();
        }
    }
    flush();
    return tokens;
}

// Symbol match tiers land in 35-85: SymbolMatch::score's own range is
// 40 (substring) .. 105 (exact + case bonus) — see SymbolSearch.cpp.
int SymbolPriority(int score) {
    return std::clamp(score * 8 / 10, 35, 85);
}

// Keyword match tiers land in 15-35 — see KeywordSearch.cpp for
// KeywordMatch::score's own range (occurrence count * 10, +5 whole-word
// bonus).
int KeywordPriority(int score) {
    return std::clamp(score, 15, 35);
}

// 0 = no match. Exact filename match ranks above a filename substring,
// which ranks above a match only in an earlier path segment (e.g. a
// directory name).
int FilePathScore(const std::string& path, const std::string& lower_intent) {
    const std::string filename = PathToUtf8(Utf8ToPath(path).filename());
    const std::string lower_filename = ToLower(filename);
    if (lower_filename == lower_intent) {
        return 60;
    }
    if (lower_filename.find(lower_intent) != std::string::npos) {
        return 45;
    }
    if (ToLower(path).find(lower_intent) != std::string::npos) {
        return 25;
    }
    return 0;
}

// Active File Bias magnitudes (see ContextRetriever.hpp class comment).
// Chosen so a Symbol match's own 35-85 range reaches exactly the
// docs/MASTER_SPEC.md #14 "100 = 現在の編集対象" ceiling when the matched
// symbol's file IS the active document (85 + 20, clamped), and lands
// near "90 = 直接依存" for a file directly included by/including it (85 +
// 10) -- deliberately proportioned against the existing tiers rather
// than picked arbitrarily, so this reads as "push toward the top of the
// scale that already existed" and not a new, disconnected scale.
constexpr int kActiveFileBias = 20;
constexpr int kActiveFileNeighborBias = 10;

// A tiny, dependency-free view over "is this file the active document,
// or does IncludeGraph say it's directly adjacent to it" -- built once
// per Retrieve() call (a single EditorStateStore::Read(), not one per
// candidate item) and consulted by every source below that has a real
// per-item file identity. Backend Context Provider items deliberately
// aren't run through this (see class comment) -- a Backend id isn't
// guaranteed to name a file at all, so biasing it by "file identity"
// isn't a meaningful operation the way it is for Symbol/Dependency/
// File/Keyword.
class ActiveFileBias {
public:
    ActiveFileBias(std::optional<std::string> active_file, const IncludeGraph* include_graph)
        : active_file_(std::move(active_file)), include_graph_(include_graph) {}

    // Adds this file's bias (if any) to `priority`, clamped to the
    // documented 0-100 ContextItem::priority scale -- never lets a boost
    // push a score past the ceiling that already means "top priority".
    [[nodiscard]] int Apply(int priority, const std::string& file_path) const {
        return std::clamp(priority + BiasFor(file_path), 0, 100);
    }

private:
    [[nodiscard]] int BiasFor(const std::string& file_path) const {
        if (!active_file_.has_value() || file_path.empty()) {
            return 0;
        }
        if (file_path == *active_file_) {
            return kActiveFileBias;
        }
        if (include_graph_ != nullptr) {
            const auto includes = include_graph_->Includes(*active_file_);
            const auto included_by = include_graph_->IncludedBy(*active_file_);
            if (std::find(includes.begin(), includes.end(), file_path) != includes.end() ||
                std::find(included_by.begin(), included_by.end(), file_path) != included_by.end()) {
                return kActiveFileNeighborBias;
            }
        }
        return 0;
    }

    std::optional<std::string> active_file_;
    const IncludeGraph* include_graph_;
};

} // namespace

std::optional<std::string> ContextRetriever::CurrentActiveDocumentPath() const {
    if (options_.editor_state_store == nullptr) {
        return std::nullopt;
    }
    const auto state = options_.editor_state_store->Read();
    if (!state.has_value()) {
        return std::nullopt;
    }
    return state->active_document_path;
}

std::vector<ContextItem> ContextRetriever::Retrieve(const std::string& intent) const {
    std::vector<ContextItem> items;
    if (intent.empty()) {
        return items;
    }

    std::unordered_set<std::string> seen_ids;
    std::unordered_set<std::string> matched_files;
    const auto add_item = [&](ContextItem item) {
        if (seen_ids.insert(item.id).second) {
            items.push_back(std::move(item));
        }
    };
    // Context Firewall (see class comment) — a nullptr firewall allows
    // everything, unchanged from before it existed.
    const auto passes_firewall = [&](const std::string& file_path) {
        return options_.firewall == nullptr || options_.firewall->IsAllowed(file_path);
    };

    // Active File Bias (see class comment): a single Read() per
    // Retrieve() call, not one per candidate — EditorStateStore::Read()
    // itself already re-reads the file fresh every call, so this is one
    // file read regardless of how many items end up scored below.
    const ActiveFileBias bias(CurrentActiveDocumentPath(), options_.include_graph);

    // Tokenized once, shared by Symbol/File/Keyword retrieval below (see
    // TokenizeIntent's own comment) -- Backend Context Provider retrieval
    // further down deliberately keeps receiving the raw `intent` string,
    // since a Backend is free to interpret free text however it likes.
    const auto intent_tokens = TokenizeIntent(intent);

    // Symbol retrieval: the strongest signal, so it runs first and
    // seeds `matched_files` for the coarser sources below to defer to.
    // Each token is searched independently and results are merged by
    // symbol identity, keeping the best score any single token achieved
    // -- the same per-source "best single match wins" heuristic File
    // retrieval below already uses.
    if (options_.symbol_index != nullptr) {
        const SymbolSearch search;
        std::unordered_map<std::string, SymbolMatch> best_by_symbol;
        for (const auto& token : intent_tokens) {
            for (auto& match : search.Search(*options_.symbol_index, token, options_.max_symbol_matches)) {
                const std::string key = match.symbol.file_path + '\x1f' + match.symbol.name;
                auto [it, inserted] = best_by_symbol.try_emplace(key, match);
                if (!inserted && match.score > it->second.score) {
                    it->second = match;
                }
            }
        }
        std::vector<SymbolMatch> merged;
        merged.reserve(best_by_symbol.size());
        for (auto& [key, match] : best_by_symbol) {
            merged.push_back(std::move(match));
        }
        std::stable_sort(merged.begin(), merged.end(),
                          [](const SymbolMatch& a, const SymbolMatch& b) { return a.score > b.score; });
        if (merged.size() > options_.max_symbol_matches) {
            merged.resize(options_.max_symbol_matches);
        }
        for (const auto& match : merged) {
            if (!passes_firewall(match.symbol.file_path)) {
                continue;
            }
            add_item(MakeSymbolContextItem(match.symbol,
                                            bias.Apply(SymbolPriority(match.score), match.symbol.file_path)));
            matched_files.insert(match.symbol.file_path);
        }
    }

    // Dependency retrieval: what each matched symbol's own file
    // includes — relevant context for editing that symbol, same
    // rationale DependencyContextSource itself documents.
    if (options_.include_graph != nullptr) {
        for (const auto& file_path : matched_files) {
            for (auto& item : MakeDependencyContextItems(*options_.include_graph, file_path,
                                                           DependencyDirection::Includes, 35, passes_firewall)) {
                item.priority = bias.Apply(item.priority, file_path);
                add_item(std::move(item));
            }
        }
    }

    if (!options_.project_root.empty()) {
        // One scan serves both File and Keyword retrieval below (they
        // used to each scan the project independently). scan_cache is
        // filled with each file's content as a side effect, so Keyword
        // retrieval's own file reads come from memory instead of disk.
        const FileScanner scanner;
        FileCache scan_cache;
        const auto scan_result = scanner.Scan(options_.project_root, &scan_cache);

        // File retrieval: path/filename substring match, skipping files
        // a symbol match already covers more precisely. Each token is
        // tried independently against every file path, keeping the best
        // score any single token achieved (see TokenizeIntent's comment).
        if (scan_result) {
            std::vector<std::string> lower_tokens;
            lower_tokens.reserve(intent_tokens.size());
            for (const auto& token : intent_tokens) {
                lower_tokens.push_back(ToLower(token));
            }
            std::vector<std::pair<int, const FileMetadata*>> scored;
            for (const auto& metadata : scan_result.Value()) {
                if (matched_files.count(metadata.path) != 0 || !passes_firewall(metadata.path)) {
                    continue;
                }
                int best_score = 0;
                for (const auto& lower_token : lower_tokens) {
                    best_score = std::max(best_score, FilePathScore(metadata.path, lower_token));
                }
                if (best_score > 0) {
                    scored.emplace_back(bias.Apply(best_score, metadata.path), &metadata);
                }
            }
            std::stable_sort(scored.begin(), scored.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });
            if (scored.size() > options_.max_file_matches) {
                scored.resize(options_.max_file_matches);
            }
            for (const auto& [score, metadata] : scored) {
                add_item(MakeFileContextItem(*metadata, score));
            }
        }

        // Keyword retrieval: the weakest, broadest signal — a fallback
        // for relevant text that symbol/file name matching missed
        // (comments, string literals, prose in non-source files). Each
        // token is searched independently and results are merged by
        // (file, line), keeping the best score any single token achieved
        // (see TokenizeIntent's comment).
        if (scan_result) {
            const KeywordSearch keyword_search;
            std::unordered_map<std::string, KeywordMatch> best_by_line;
            for (const auto& token : intent_tokens) {
                const auto keyword_result = keyword_search.Search(scan_result.Value(), options_.project_root, token,
                                                                    options_.max_keyword_matches, &scan_cache);
                if (!keyword_result) {
                    continue;
                }
                for (auto& match : keyword_result.Value()) {
                    const std::string key = match.file_path + '\x1f' + std::to_string(match.line);
                    auto [it, inserted] = best_by_line.try_emplace(key, match);
                    if (!inserted && match.score > it->second.score) {
                        it->second = match;
                    }
                }
            }
            std::vector<KeywordMatch> merged;
            merged.reserve(best_by_line.size());
            for (auto& [key, match] : best_by_line) {
                merged.push_back(std::move(match));
            }
            std::stable_sort(merged.begin(), merged.end(),
                              [](const KeywordMatch& a, const KeywordMatch& b) { return a.score > b.score; });
            if (merged.size() > options_.max_keyword_matches) {
                merged.resize(options_.max_keyword_matches);
            }
            for (const auto& match : merged) {
                if (matched_files.count(match.file_path) != 0 || !passes_firewall(match.file_path)) {
                    continue;
                }
                ContextItem item;
                item.id = "keyword:" + match.file_path + ":" + std::to_string(match.line);
                item.source = ContextSourceKind::Custom;
                item.compression = CompressionLevel::Reference;
                item.priority = bias.Apply(KeywordPriority(match.score), match.file_path);
                item.content = match.file_path + ":" + std::to_string(match.line) + ": " + match.text;
                item.estimated_tokens = EstimateTokens(item.content);
                item.depends_on = {match.file_path};
                add_item(std::move(item));
            }
        }
    }

    // Backend Context Provider retrieval (see class comment) — every
    // registered Backend gets a chance to contribute, native or Plugin
    // alike; IBackend::ProvideContext()'s own default returns nothing,
    // so a Backend that doesn't override it costs one empty-vector call.
    if (options_.backend_registry != nullptr) {
        for (const auto& backend : options_.backend_registry->All()) {
            for (auto& item : backend->ProvideContext(intent)) {
                if (!passes_firewall(item.id)) {
                    continue;
                }
                add_item(std::move(item));
            }
        }
    }

    return items;
}

} // namespace aistudio::core
