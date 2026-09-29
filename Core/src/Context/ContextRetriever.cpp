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
#include <cmath>
#include <filesystem>
#include <limits>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace aistudio::core {

namespace {

// SymbolSearch::Search/KeywordSearch::Search both already scan and sort
// their FULL candidate set before truncating to `max_results` (see their
// own implementations) -- the cap costs them nothing to lift. Passing
// options_.max_*_matches per token here used to pre-truncate each token's
// own results BEFORE the per-intent merge below, which (found via manual
// testing, 2026-09-18) both silently dropped matches a truncation flag
// could never observe (the final merged count could land under the cap
// even though a real cut happened per token) and could rank a worse match
// from one token above a better one from another (each token's pool was
// already capped independently, so a genuinely higher-scoring match
// beyond one token's own top-N never reached the merge step to compete).
// kNoPerTokenLimit keeps each token's search uncapped; only the merged,
// cross-token result is capped by options_.max_*_matches below.
constexpr std::size_t kNoPerTokenLimit = std::numeric_limits<std::size_t>::max();

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
// Tokens shorter than 3 characters and English function words are
// dropped: measured on this repository (2026-09-18), "is"/"how"/"the"
// from an ordinary "how X is implemented" intent substring-matched
// aistudio.config, CMakeLists.txt and dozens of unrelated lines, and
// those noise candidates then crowded real matches out of the response
// budget.
bool IsStopWord(const std::string& lower_token) {
    static const std::unordered_set<std::string> kStopWords = {
        "the", "and", "are", "was", "were", "been", "does", "did", "how", "what", "which", "where",
        "when", "why", "who", "for", "from", "with", "into", "this", "that", "these", "those",
        "its", "not", "you", "your", "please", "show", "here", "there", "explain", "about", "via",
        "using", "way", "works", "implemented", "implementation", "codebase", "cpp", "hpp",
    };
    return kStopWords.count(lower_token) != 0;
}

// Something the author typed as a name rather than a word: an inner
// capital (CamelCase), an underscore, or a digit. "IsAllowed" and
// "symbol_search" qualify; "firewall" and "restricting" do not.
bool LooksLikeIdentifier(const std::string& token) {
    for (std::size_t i = 1; i < token.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        if (std::isupper(c) != 0 || std::isdigit(c) != 0 || c == '_') {
            return true;
        }
    }
    return false;
}

// Every token is tried against every source, so a long intent turns
// into a wide net: measured on this repository (2026-09-18), a model
// that missed once wrote a 12-word intent next, and the hits from its
// plain-English words drowned the ones from the identifiers it named.
// Identifier-looking tokens go first, then longer words, and only the
// top kMaxIntentTokens are used.
constexpr std::size_t kMaxIntentTokens = 6;

// The token plus rough English stems of it, for symbol-name matching:
// SymbolSearch is a substring match, and a plural or inflected intent
// word is LONGER than the identifier it means ("migrations" never occurs
// inside "Migrator" or "Migration"; "migrat" occurs in both). Only
// suffixes that leave a stem of 4+ characters, so short words stay as
// typed. Deliberately not a real stemmer -- identifiers are not prose.
std::vector<std::string> StemVariants(const std::string& token) {
    std::vector<std::string> variants{token};
    static const char* const kSuffixes[] = {"ions", "ion", "ings", "ing", "ies", "ed", "es", "s"};
    for (const char* suffix : kSuffixes) {
        const std::string_view sv(suffix);
        if (token.size() > sv.size() + 3 && token.compare(token.size() - sv.size(), sv.size(), sv) == 0) {
            std::string stem = token.substr(0, token.size() - sv.size());
            if (sv == "ies") {
                stem += 'y';
            }
            if (std::find(variants.begin(), variants.end(), stem) == variants.end()) {
                variants.push_back(std::move(stem));
            }
        }
    }
    return variants;
}

// Whitespace-delimited chunks of the intent that carry punctuation
// INSIDE them -- "tools/call", "std::vector", "foo->bar", "a.b.c".
// TokenizeIntent splits those apart, which is right for symbol names but
// loses the thing itself: "tools/call" occurs verbatim in the one line
// that dispatches a tools/call request, while its halves occur in
// hundreds of lines. Keyword retrieval searches these as written.
// KeywordPriority's own ceiling caps what this can win.
constexpr int kPhraseHitBonus = 40;

std::vector<std::string> IntentPhrases(const std::string& intent) {
    std::vector<std::string> phrases;
    std::string current;
    const auto flush = [&]() {
        // Trim punctuation at the ends (sentence commas, quotes); what is
        // left must still contain punctuation to be a phrase rather than a
        // plain token TokenizeIntent already covers.
        const auto is_word = [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; };
        std::size_t first = 0;
        while (first < current.size() && !is_word(static_cast<unsigned char>(current[first]))) {
            ++first;
        }
        std::size_t last = current.size();
        while (last > first && !is_word(static_cast<unsigned char>(current[last - 1]))) {
            --last;
        }
        const std::string trimmed = current.substr(first, last - first);
        current.clear();
        if (trimmed.size() < 4) {
            return;
        }
        const bool has_punctuation =
            std::any_of(trimmed.begin(), trimmed.end(),
                        [&](char c) { return !is_word(static_cast<unsigned char>(c)); });
        if (has_punctuation && std::find(phrases.begin(), phrases.end(), trimmed) == phrases.end()) {
            phrases.push_back(trimmed);
        }
    };
    for (const char c : intent) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            flush();
        } else {
            current.push_back(c);
        }
    }
    flush();
    return phrases;
}

std::vector<std::string> TokenizeIntent(const std::string& intent) {
    std::vector<std::string> tokens;
    std::string current;
    const auto flush = [&]() {
        if (current.size() >= 3 && !IsStopWord(ToLower(current)) &&
            std::find(tokens.begin(), tokens.end(), current) == tokens.end()) {
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
    if (tokens.size() > kMaxIntentTokens) {
        std::stable_sort(tokens.begin(), tokens.end(), [](const std::string& a, const std::string& b) {
            const bool a_id = LooksLikeIdentifier(a);
            const bool b_id = LooksLikeIdentifier(b);
            if (a_id != b_id) {
                return a_id;
            }
            return a.size() > b.size();
        });
        tokens.resize(kMaxIntentTokens);
    }
    return tokens;
}

// Symbol match tiers land in 35-85: SymbolMatch::score's own range is
// 40 (substring) .. 105 (exact + case bonus) — see SymbolSearch.cpp.
// SymbolMatch::score runs 40 (substring) .. 105 (exact + case bonus) --
// see SymbolSearch.cpp -- and this spreads that across the 35-85 Symbol
// tier linearly.
//
// It used to be score * 8 / 10 clamped to the same range, which sent
// EVERY substring match to the floor: 40 * 0.8 = 32, clamped up to 35,
// the same priority a barely-relevant match gets. Measured 2026-09-24,
// that made a whole class of match unable to compete -- an intent about
// compression ranked the member field literally named `compression`
// (exact match, 100) far above SelectWithCompression (substring, 50),
// which is where compression actually happens, and no amount of extra
// slots helped because the substring match was pinned to the floor.
int SymbolPriority(int score) {
    constexpr int kMinScore = 40;
    constexpr int kMaxScore = 105;
    constexpr int kMinPriority = 35;
    constexpr int kMaxPriority = 85;
    const int clamped = std::clamp(score, kMinScore, kMaxScore);
    return kMinPriority +
            (clamped - kMinScore) * (kMaxPriority - kMinPriority) / (kMaxScore - kMinScore);
}

// See the Symbol retrieval block in Retrieve() for what these adjust.
// The result is still clamped to the 35-85 Symbol tier, so the Active
// File Bias arithmetic documented in the class comment is unchanged.
constexpr int kMultiTokenBonus = 10;
constexpr int kVariablePenalty = 30;

// How much one intent token's symbol matches count, by how many symbols
// it matched: a token that names a handful of symbols is a signal, one
// that names a hundred is a prefix. Measured on this repository
// (2026-09-18), "context firewall" spent all five Symbol slots on
// Context* classes ("context" is this project's most common prefix) and
// never reached the one class "firewall" was about. 1.0 up to
// kSpecificTokenMatches matches, then falls off logarithmically to a
// floor -- never to zero, so a generic token still breaks ties.
constexpr std::size_t kSpecificTokenMatches = 5;
constexpr double kGenericTokenFloor = 0.3;

double TokenSpecificity(std::size_t match_count) {
    if (match_count <= kSpecificTokenMatches) {
        return 1.0;
    }
    const double excess = std::log2(static_cast<double>(match_count) / static_cast<double>(kSpecificTokenMatches));
    return std::max(kGenericTokenFloor, 1.0 - 0.15 * excess);
}

// A class/namespace body is its member list: the first lines carry the
// API and the rest is detail, so it gets a shorter cap than a function
// (kMaxBodyLines), whose logic can sit anywhere in it.
constexpr int kMaxClassBodyLines = 40;

// A File item at or under this size is sent whole instead of as a
// fetch-me stub -- see the File retrieval block in Retrieve(). ~6 KB is
// a header or a small implementation file (~1.5k tokens), an amount the
// response budget can absorb; anything larger would crowd out the
// symbol matches that answer the intent directly.
constexpr std::uint64_t kInlineFileMaxBytes = 6144;

// A symbol match in a file this small brings the whole file with it --
// see the Symbol retrieval block in Retrieve(). 6 KB is an ordinary
// implementation file; the point is that its OTHER functions are what
// the caller would otherwise spend a turn fetching.
constexpr std::uint64_t kWholeFileForSymbolBytes = 6144;

// ...for at most this many files. Without a cap, an intent whose words
// appear in several small files promoted all of them and filled the
// whole response budget with files (measured 2026-09-23: 29k characters,
// and the caller still made four more calls). Later matches fall back to
// their own declaration, which is what they were before.
constexpr int kMaxWholeFilesFromSymbols = 2;

// ...and only when the match was on the file's NAME (FilePathScore 45 =
// filename substring, 60 = exact) rather than on an enclosing directory
// (25). See the File retrieval block in Retrieve().
constexpr int kInlineFileMinPathScore = 45;

// ...and only for the first few. When several files match the intent's
// words equally well, none of them is "the" file, and sending all of
// them whole is speculation the caller pays for on every later turn:
// measured 2026-09-23, four inlined files were 49% of a response for a
// task the grep-and-read baseline answered more cheaply. The rest stay
// as stubs, which name the file and its size so the caller can fetch
// the one it decides it wants.
constexpr int kMaxInlinedFiles = 2;

// Backend Context Provider items are capped at this priority — see the
// Backend retrieval block in Retrieve().
constexpr int kBackendPriorityCap = 10;

// See the Relative cutoff block at the end of Retrieve(). 30 spans one
// full tier of the 0-100 priority scale: with a strong symbol match
// (85) it keeps symbol and file matches and drops keyword-floor noise
// (35), while a weak best match (45) keeps everything down to 15.
constexpr int kRelativePriorityCutoff = 30;

// At most this many include edges per response -- see Dependency
// retrieval in Retrieve().
constexpr std::size_t kMaxDependencyEdges = 8;

// How many below-cutoff keyword hits may survive because they point at
// a file the response does not otherwise contain -- see the Relative
// cutoff block at the end of Retrieve().
constexpr std::size_t kMaxExemptedCallSites = 3;

// A definition body longer than this is cut here, before ContextSelector
// ever sees it: a 400-line class is not a better answer than its first
// 120 lines plus a marker, and the head/tail cut ContextCompressor would
// otherwise make on overflow drops the middle, which for code is the
// part that matters.
constexpr int kMaxBodyLines = 120;

// Upgrades a signature-only Symbol item into one carrying the actual
// definition (lines Symbol::line..Symbol::end_line of the file, from the
// scan snapshot's in-memory contents). Without this, context_retrieve
// was a table of contents: measured on this repository (2026-09-18) the
// model followed every Symbol item with a context_fetch of the whole
// file, paying for the manifest AND the file, and the "core" run cost
// more context than plain Read/Grep did. Variables keep the signature
// only (their "body" is the declaration already in the signature).
// Lines first_line..last_line (1-based, inclusive) of `content`, each
// with its trailing newline; at most `max_lines`, then a marker with the
// count left out. Empty if the range is off the end of the file.
struct LineSlice {
    std::string text;
    bool cut = false;
};

LineSlice SliceLines(const std::string& content, int first_line, int last_line, int max_lines) {
    LineSlice slice;
    int current_line = 1;
    int emitted = 0;
    std::size_t pos = 0;
    while (pos <= content.size() && current_line <= last_line) {
        const auto next = content.find('\n', pos);
        const auto line_end = next == std::string::npos ? content.size() : next;
        if (current_line >= first_line) {
            if (emitted == max_lines) {
                slice.text += "... [" + std::to_string(last_line - current_line + 1) + " more lines]\n";
                slice.cut = true;
                break;
            }
            slice.text.append(content, pos, line_end - pos);
            slice.text += '\n';
            ++emitted;
        }
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
        ++current_line;
    }
    return slice;
}

std::optional<std::string> FileContent(const std::string& file_path, FileCache& contents,
                                       const std::unordered_map<std::string, const FileMetadata*>& metadata_by_path) {
    const auto metadata_it = metadata_by_path.find(file_path);
    if (metadata_it == metadata_by_path.end()) {
        return std::nullopt;
    }
    auto content = contents.Get(*metadata_it->second);
    // Not every project saves its source as UTF-8. Measured on the
    // GameEngine repository (2026-09-29): context_retrieve returned
    // nothing but "[json.exception.type_error.316] invalid UTF-8 byte at
    // index 273: 0x82" -- one CP932 file anywhere in the response makes
    // nlohmann's dump() throw and discards the WHOLE response, so the
    // model fell back to 10 manual Read/Grep calls. context_fetch
    // (McpServer.cpp) already did this fallback; retrieval did not.
    if (content.has_value() && !IsValidUtf8(*content)) {
        return Cp932ToUtf8(*content); // nullopt => genuinely binary, as before
    }
    return content;
}

// Returns true when the WHOLE definition went in -- a keyword hit inside
// that range is then already visible and needs no snippet of its own.
// False when nothing was attached or the body was cut at kMaxBodyLines
// (the part after the cut is exactly where a keyword hit in a 1000-line
// function tends to be).
bool AttachDefinitionBody(ContextItem& item, const Symbol& symbol, FileCache& contents,
                          const std::unordered_map<std::string, const FileMetadata*>& metadata_by_path) {
    if (symbol.kind == SymbolKind::Variable || symbol.end_line < symbol.line || symbol.line <= 0) {
        return false;
    }
    const auto content = FileContent(symbol.file_path, contents, metadata_by_path);
    if (!content.has_value()) {
        return false;
    }
    const int max_lines = symbol.kind == SymbolKind::Function ? kMaxBodyLines : kMaxClassBodyLines;
    const auto body = SliceLines(*content, symbol.line, symbol.end_line, max_lines);
    if (body.text.empty()) {
        return false;
    }

    item.content = ToString(symbol.kind) + " " + symbol.name + " (" + symbol.file_path + ":" +
                   std::to_string(symbol.line) + "-" + std::to_string(symbol.end_line) + ")\n" + body.text;
    item.compression = CompressionLevel::Raw;
    item.estimated_tokens = EstimateTokens(item.content);
    return !body.cut;
}

// A definition whose full body was sent, so keyword hits inside it are
// redundant.
struct CoveredRange {
    std::string file_path;
    int first_line = 0;
    int last_line = 0;
};

// Keyword hits come back as a window of source around the line, not the
// line alone: measured on this repository (2026-09-18), the answer to
// "where does X get checked" sat inside a 1000-line dispatch function,
// which no Symbol item can carry whole, and a bare "file:line: text" hit
// left the model fetching line ranges by trial and error (20 tool calls
// for one question). Hits in the same file closer than a window apart
// merge into one snippet so the same lines are never sent twice.
constexpr int kKeywordWindowLines = 6;

// Test sources match almost any identifier the intent names (every test
// spells the thing under test out), but for "how does X work" they are
// the least informative hit -- ranked below the same hit in real source.
constexpr int kTestFilePenalty = 10;

bool LooksLikeTestFile(const std::string& path) {
    return path.find("/tests/") != std::string::npos || path.find("/test/") != std::string::npos ||
           path.find("test_") != std::string::npos;
}

// Ordinary keyword match tiers land in 15-35 — see KeywordSearch.cpp
// for KeywordMatch::score's own range (occurrence count * 10, +5
// whole-word bonus). A phrase hit (see IntentPhrases) is allowed past
// that ceiling, up to the Symbol tier's floor: it is the most specific
// evidence available -- the intent's own punctuated string, found
// verbatim in the source -- and at 35 it lost every slot to symbols that
// merely shared a common word.
int KeywordPriority(int rank) {
    return std::clamp(rank, 15, rank >= kPhraseHitBonus ? 60 : 35);
}

// A keyword match plus where the query that found it came from. Phrase
// hits are ranked above every ordinary hit rather than by occurrence
// count, which is what "specific" means here.
struct KeywordHit {
    KeywordMatch match;
    bool from_phrase = false;
};

int HitRank(const KeywordHit& hit) {
    return hit.match.score + (hit.from_phrase ? kPhraseHitBonus : 0);
}

// A file path lower-cased once per Retrieve() (not once per token as
// FilePathScore used to do internally -- the UTF-8 <-> std::filesystem
// round trip for filename() is the expensive part, and it was being
// repeated files x tokens times).
struct LoweredPath {
    std::string lower_path;
    std::string lower_filename;
};

LoweredPath LowerPath(const std::string& path) {
    return LoweredPath{
        .lower_path = ToLower(path),
        .lower_filename = ToLower(PathToUtf8(Utf8ToPath(path).filename())),
    };
}

// 0 = no match. Exact filename match ranks above a filename substring,
// which ranks above a match only in an earlier path segment (e.g. a
// directory name).
int FilePathScore(const LoweredPath& path, const std::string& lower_intent) {
    if (path.lower_filename == lower_intent) {
        return 60;
    }
    if (path.lower_filename.find(lower_intent) != std::string::npos) {
        return 45;
    }
    if (path.lower_path.find(lower_intent) != std::string::npos) {
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

// How many DISTINCT intent tokens a file's path/name matches. A file
// named after two of the intent's words is what the question is about:
// "how the MCP server dispatches a tools/call request" is asked of
// McpServer.cpp, but no symbol in it is called "dispatch" (the symbols
// that are -- BackendRegistry::Dispatch and friends -- answer a
// different question). Measured on this repository (2026-09-22), that
// intent returned five unrelated Dispatch functions and never CallTool.
// Only counts from the second matched token on, so a file matching one
// ordinary word gains nothing.
constexpr int kFileAffinityBonus = 12;
constexpr int kMaxFileAffinityTokens = 3;

int FileAffinity(const LoweredPath& path, const std::vector<std::string>& lower_tokens) {
    int matched = 0;
    for (const auto& token : lower_tokens) {
        if (FilePathScore(path, token) > 0) {
            ++matched;
        }
    }
    return kFileAffinityBonus * std::min(std::max(matched - 1, 0), kMaxFileAffinityTokens - 1);
}

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

std::shared_ptr<ContextRetriever::ScanSnapshot> ContextRetriever::ScanProject() const {
    std::uint64_t generation = 0;
    if (options_.cache_scan) {
        std::lock_guard lock(scan_mutex_);
        if (scan_snapshot_ != nullptr) {
            return scan_snapshot_;
        }
        generation = scan_generation_;
    }

    auto snapshot = std::make_shared<ScanSnapshot>();
    const FileScanner scanner(FileScanner::MakeOptions(options_.extra_ignore_patterns));
    auto scan_result = scanner.Scan(options_.project_root, &snapshot->contents);
    if (!scan_result) {
        return nullptr;
    }
    snapshot->files = std::move(scan_result.Value());

    if (options_.cache_scan) {
        std::lock_guard lock(scan_mutex_);
        // An InvalidateScan() that landed while this scan was on disk
        // means the scan may already be stale -- serve it to this one
        // caller (same as an uncached call would have) but don't cache it.
        if (scan_generation_ == generation) {
            scan_snapshot_ = snapshot;
        }
    }
    return snapshot;
}

void ContextRetriever::InvalidateScan() {
    std::lock_guard lock(scan_mutex_);
    scan_snapshot_.reset();
    ++scan_generation_;
}

std::vector<ContextItem> ContextRetriever::Retrieve(const std::string& intent, RetrievalTruncation* truncation) const {
    std::vector<ContextItem> items;
    if (intent.empty()) {
        return items;
    }

    std::unordered_set<std::string> seen_ids;
    std::unordered_set<std::string> matched_files;
    std::vector<CoveredRange> covered;
    // Matched files in match order -- matched_files is unordered, and
    // Dependency retrieval below needs the best match first.
    std::vector<std::string> dependency_order;
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

    // One scan serves Symbol bodies, File and Keyword retrieval below
    // (they used to each scan the project independently), and with
    // Options::cache_scan it serves later Retrieve() calls too.
    // snapshot->contents is filled with each file's content as a side
    // effect of the scan, so every read below comes from memory.
    const auto snapshot = options_.project_root.empty() ? nullptr : ScanProject();
    std::unordered_map<std::string, const FileMetadata*> metadata_by_path;
    std::vector<std::string> lower_tokens;
    lower_tokens.reserve(intent_tokens.size());
    for (const auto& token : intent_tokens) {
        lower_tokens.push_back(ToLower(token));
    }
    std::unordered_map<std::string, LoweredPath> lowered_by_path;
    std::unordered_map<std::string, int> affinity_by_path;
    if (snapshot != nullptr) {
        for (const auto& metadata : snapshot->files) {
            metadata_by_path.emplace(metadata.path, &metadata);
            auto lowered = LowerPath(metadata.path);
            affinity_by_path.emplace(metadata.path, FileAffinity(lowered, lower_tokens));
            lowered_by_path.emplace(metadata.path, std::move(lowered));
        }
    }
    const auto affinity_of = [&](const std::string& file_path) {
        const auto it = affinity_by_path.find(file_path);
        return it == affinity_by_path.end() ? 0 : it->second;
    };

    const auto whole_file_for = [&](const std::string& file_path) -> const FileMetadata* {
        if (snapshot == nullptr) {
            return nullptr;
        }
        if (LooksLikeTestFile(file_path)) {
            // A test is the least informative answer to "how does this
            // work" (see kTestFilePenalty), so it never earns the
            // whole-file treatment. Found 2026-09-24: a 10 KB test came
            // back whole for a question whose answer cited none of it.
            return nullptr;
        }
        const auto it = metadata_by_path.find(file_path);
        return it != metadata_by_path.end() && it->second->size <= kWholeFileForSymbolBytes ? it->second
                                                                                             : nullptr;
    };

    // Symbol retrieval: the strongest signal, so it runs first and
    // seeds `matched_files` for the coarser sources below to defer to.
    // Each token is searched independently and results are merged by
    // symbol identity. Ranking = best single-token score, plus a bonus
    // per additional distinct token the same symbol matched (a symbol
    // named by two intent words is a better answer than one named by
    // one: "symbol search" -> SymbolSearch over a field named `symbol`),
    // minus a penalty for Variables (a field is rarely what "how does X
    // work" is asking about, but its short name exact-matches easily).
    if (options_.symbol_index != nullptr) {
        const SymbolSearch search;
        struct MergedMatch {
            SymbolMatch match;
            int matched_tokens = 0;
            // Sum of TokenSpecificity over the tokens that matched, so
            // the multi-token bonus below can tell "named after two rare
            // words" from "named after two of this project's commonest
            // ones".
            double specificity_sum = 0.0;
        };
        std::unordered_map<std::string, MergedMatch> best_by_symbol;
        for (const auto& token : intent_tokens) {
            // One token's variants (see StemVariants) pool into one result
            // set, so "migrations" counts as a single matched token for the
            // multi-token bonus whichever spelling hit.
            std::unordered_map<std::string, SymbolMatch> token_best;
            for (const auto& variant : StemVariants(token)) {
                for (auto& match : search.Search(*options_.symbol_index, variant, kNoPerTokenLimit)) {
                    const std::string key = match.symbol.file_path + '\x1f' + match.symbol.name;
                    auto [it, inserted] = token_best.try_emplace(key, match);
                    if (!inserted && match.score > it->second.score) {
                        it->second = match;
                    }
                }
            }
            const double weight = TokenSpecificity(token_best.size());
            for (auto& [key, match] : token_best) {
                match.score = static_cast<int>(match.score * weight);
                auto [it, inserted] = best_by_symbol.try_emplace(key, MergedMatch{match, 1, weight});
                if (!inserted) {
                    ++it->second.matched_tokens;
                    it->second.specificity_sum += weight;
                    if (match.score > it->second.match.score) {
                        it->second.match = match;
                    }
                }
            }
        }
        // A matched Variable's declared type is usually what the intent was
        // really about ("firewall" -> the five `const Sandbox* firewall`
        // options, but the answer is class Sandbox). Symbol::type_name was
        // captured for exactly this resolution; the type's definition joins
        // the pool at the variable's own score so it competes on equal
        // terms and, being a Class, wins the tie the Variable penalty
        // creates below.
        for (const auto& [key, entry] : std::vector<std::pair<std::string, MergedMatch>>(best_by_symbol.begin(),
                                                                                          best_by_symbol.end())) {
            if (entry.match.symbol.kind != SymbolKind::Variable || entry.match.symbol.type_name.empty()) {
                continue;
            }
            for (const auto& type_symbol : options_.symbol_index->FindByName(entry.match.symbol.type_name)) {
                if (type_symbol.kind == SymbolKind::Variable) {
                    continue;
                }
                const std::string type_key = type_symbol.file_path + '\x1f' + type_symbol.name;
                auto [it, inserted] = best_by_symbol.try_emplace(
                    type_key, MergedMatch{SymbolMatch{type_symbol, entry.match.score}, entry.matched_tokens,
                                           entry.specificity_sum});
                if (!inserted && entry.match.score > it->second.match.score) {
                    it->second.match.score = entry.match.score;
                }
            }
        }
        std::vector<std::pair<int, SymbolMatch>> merged;
        merged.reserve(best_by_symbol.size());
        for (auto& [key, entry] : best_by_symbol) {
            // The bonus for matching several intent words is scaled by how
            // specific those words were. Measured 2026-09-24: an intent
            // about ranking, compression and budgeting put ContextItem and
            // ContextBudget on top -- each named after two of this
            // project's commonest words ("context" plus "item"/"budget")
            // -- while SelectWithCompression, which is where the ranking
            // actually happens, matched only the rare word and never made
            // the cut. Unweighted, two generic words beat one precise one.
            const double specificity =
                entry.matched_tokens > 0 ? entry.specificity_sum / entry.matched_tokens : 1.0;
            int priority = SymbolPriority(entry.match.score) +
                            static_cast<int>(kMultiTokenBonus * (entry.matched_tokens - 1) * specificity);
            if (entry.match.symbol.kind == SymbolKind::Variable) {
                priority -= kVariablePenalty;
            }
            if (LooksLikeTestFile(entry.match.symbol.file_path)) {
                priority -= kTestFilePenalty;
            }
            priority += affinity_of(entry.match.symbol.file_path);
            merged.emplace_back(std::clamp(priority, 35, 85), std::move(entry.match));
        }
        std::stable_sort(merged.begin(), merged.end(),
                          [](const auto& a, const auto& b) { return a.first > b.first; });
        if (merged.size() > options_.max_symbol_matches) {
            merged.resize(options_.max_symbol_matches);
            if (truncation != nullptr) {
                truncation->symbol = true;
            }
        }
        // A match in a small file brings the WHOLE file instead of just
        // its own declaration. The function that answers the question is
        // often a sibling of the one whose name matched: "how does the
        // context cache memoize" matches Invalidate and InvalidateAll
        // (two lines each) while the memoizing happens in GetOrRetrieve,
        // whose name contains no word from the intent. Measured
        // 2026-09-23, all three tasks still losing spent their follow-up
        // calls fetching the matched symbol's own file for exactly that
        // reason -- and those files were 2-5 KB, less than the round trip
        // cost. The file supersedes the symbol item rather than joining
        // it, since it already contains that declaration.

        int whole_files = 0;
        std::unordered_set<std::string> sent_whole;
        for (const auto& [priority, match] : merged) {
            if (!passes_firewall(match.symbol.file_path)) {
                continue;
            }
            const bool first_from_file = matched_files.insert(match.symbol.file_path).second;
            if (first_from_file) {
                dependency_order.push_back(match.symbol.file_path);
            }

            if (sent_whole.count(match.symbol.file_path) != 0) {
                // This file already went out whole, so emitting one of its
                // declarations again sends the same bytes twice. Found
                // 2026-09-24 by diffing what one response contained
                // against what the answer cited: a function body appeared
                // both inside its file and as its own item, because the
                // whole-file cap had been reached by the time the second
                // match from that file came up.
                continue;
            }
            const FileMetadata* metadata =
                whole_files < kMaxWholeFilesFromSymbols ? whole_file_for(match.symbol.file_path) : nullptr;
            if (metadata != nullptr) {
                if (!first_from_file) {
                    continue; // the file is already in the response
                }
                if (auto content = FileContent(match.symbol.file_path, snapshot->contents, metadata_by_path)) {
                    ++whole_files;
                    sent_whole.insert(match.symbol.file_path);
                    covered.push_back({match.symbol.file_path, 1, std::numeric_limits<int>::max()});
                    add_item(MakeFileContextItem(*metadata, *std::move(content),
                                                  bias.Apply(priority, match.symbol.file_path)));
                    continue;
                }
            }

            auto item = MakeSymbolContextItem(match.symbol, bias.Apply(priority, match.symbol.file_path));
            if (snapshot != nullptr &&
                AttachDefinitionBody(item, match.symbol, snapshot->contents, metadata_by_path)) {
                covered.push_back({match.symbol.file_path, match.symbol.line, match.symbol.end_line});
            }
            add_item(std::move(item));
        }
    }

    // Dependency retrieval: what each matched symbol's own file
    // includes — relevant context for editing that symbol, same
    // rationale DependencyContextSource itself documents.
    //
    // Capped, and taken in match order rather than from the unordered
    // set of matched files: every include of every matched file is a
    // dump, not a map. Measured 2026-09-23, it reached 15% of one
    // response (roughly thirty "a.cpp includes b.hpp" lines) for a
    // question about runtime behaviour, where the includes of the
    // best-matching file or two are the part that says anything.
    if (options_.include_graph != nullptr) {
        std::size_t edges = 0;
        for (const auto& file_path : dependency_order) {
            if (edges >= kMaxDependencyEdges) {
                break;
            }
            for (auto& item : MakeDependencyContextItems(*options_.include_graph, file_path,
                                                           DependencyDirection::Includes, 35, passes_firewall)) {
                if (edges >= kMaxDependencyEdges) {
                    break;
                }
                item.priority = bias.Apply(item.priority, file_path);
                add_item(std::move(item));
                ++edges;
            }
        }
    }

    if (snapshot != nullptr) {
        // File retrieval: path/filename substring match, skipping files
        // a symbol match already covers more precisely. Each token is
        // tried independently against every file path, keeping the best
        // score any single token achieved (see TokenizeIntent's comment).
        {
            struct ScoredFile {
                int priority = 0;
                int path_score = 0; // pre-bias FilePathScore: how the path matched
                const FileMetadata* metadata = nullptr;
            };
            std::vector<ScoredFile> scored;
            for (const auto& metadata : snapshot->files) {
                if (matched_files.count(metadata.path) != 0 || !passes_firewall(metadata.path)) {
                    continue;
                }
                const LoweredPath& lowered = lowered_by_path.at(metadata.path);
                int best_score = 0;
                for (const auto& lower_token : lower_tokens) {
                    best_score = std::max(best_score, FilePathScore(lowered, lower_token));
                }
                if (best_score > 0) {
                    scored.push_back({bias.Apply(best_score + affinity_of(metadata.path), metadata.path), best_score,
                                      &metadata});
                }
            }
            std::stable_sort(scored.begin(), scored.end(),
                              [](const ScoredFile& a, const ScoredFile& b) { return a.priority > b.priority; });
            if (scored.size() > options_.max_file_matches) {
                scored.resize(options_.max_file_matches);
                if (truncation != nullptr) {
                    truncation->file = true;
                }
            }
            // Small files go in whole rather than as a fetch-me stub.
            // A stub costs ~30 tokens and buys a follow-up context_fetch,
            // and a follow-up costs a whole extra turn -- which re-sends
            // the entire conversation so far (10-25k tokens by then).
            // Measured across this repository's bench tasks (2026-09-22),
            // reduction tracks tool-call count far more strongly than
            // response size: the one-call task cut 74%, the nine-call one
            // 18%. Paying a few hundred tokens to remove a turn is the
            // right trade; a big file is not (it would crowd out
            // everything else in the budget), so it still gets a stub.
            //
            // Only files whose NAME matched an intent word are worth that
            // trade (FilePathScore: 60 exact, 45 substring; 25 means only
            // some earlier path segment matched, e.g. every file under
            // Core/Context/ for the word "context"). Inlining those too
            // spent 72% of one response on six files where two were the
            // answer (measured 2026-09-22).
            int inlined_files = 0;
            for (const auto& entry : scored) {
                const bool worth_inlining = inlined_files < kMaxInlinedFiles &&
                                            entry.path_score >= kInlineFileMinPathScore &&
                                            entry.metadata->size <= kInlineFileMaxBytes &&
                                            !LooksLikeTestFile(entry.metadata->path);
                auto content = worth_inlining
                                   ? FileContent(entry.metadata->path, snapshot->contents, metadata_by_path)
                                   : std::nullopt;
                if (content.has_value()) {
                    ++inlined_files;
                    // The whole file is in the response, so Keyword
                    // retrieval below must not bill a window of it again
                    // as a separate item -- unlike a symbol body, which
                    // covers only its own lines (see CoveredRange).
                    covered.push_back({entry.metadata->path, 1, std::numeric_limits<int>::max()});
                }
                add_item(content.has_value()
                             ? MakeFileContextItem(*entry.metadata, *std::move(content), entry.priority)
                             : MakeFileContextItem(*entry.metadata, entry.priority));
            }
        }

        // Keyword retrieval: the weakest, broadest signal — a fallback
        // for relevant text that symbol/file name matching missed
        // (comments, string literals, prose in non-source files). Every
        // token is searched in one pass over the files (KeywordSearch's
        // multi-query overload) and results are merged by (file, line),
        // keeping the best score any single token achieved (see
        // TokenizeIntent's comment).
        {
            // Punctuated phrases (see IntentPhrases) are searched
            // alongside the plain tokens and their hits outrank them:
            // "tools/call" occurs in exactly the line that dispatches it,
            // while "tools" and "call" occur everywhere.
            auto queries = intent_tokens;
            const auto phrases = IntentPhrases(intent);
            queries.insert(queries.end(), phrases.begin(), phrases.end());
            const std::size_t first_phrase = intent_tokens.size();

            const KeywordSearch keyword_search;
            std::unordered_map<std::string, KeywordHit> best_by_line;
            if (auto keyword_result = keyword_search.Search(snapshot->files, options_.project_root, queries,
                                                            kNoPerTokenLimit, &snapshot->contents)) {
                auto& per_query = keyword_result.Value();
                for (std::size_t q = 0; q < per_query.size(); ++q) {
                    const bool from_phrase = q >= first_phrase;
                    for (auto& match : per_query[q]) {
                        const std::string key = match.file_path + '\x1f' + std::to_string(match.line);
                        KeywordHit hit{match, from_phrase};
                        auto [it, inserted] = best_by_line.try_emplace(key, hit);
                        if (!inserted && HitRank(hit) > HitRank(it->second)) {
                            it->second = hit;
                        }
                    }
                }
            }
            std::vector<KeywordHit> merged;
            merged.reserve(best_by_line.size());
            for (auto& [key, hit] : best_by_line) {
                merged.push_back(std::move(hit));
            }
            // Ranked by what the item's priority will actually be --
            // file affinity and the test-file penalty included -- because
            // max_keyword_matches cuts the list here. Ranking on the raw
            // score first made every phrase hit tie, and the cut then
            // kept whichever ties came out of the hash map (measured
            // 2026-09-22: the one line that dispatches a tools/call
            // request lost its slot to a test that mentions it).
            const auto hit_priority = [&](const KeywordHit& hit) {
                int priority = KeywordPriority(HitRank(hit)) + affinity_of(hit.match.file_path);
                return LooksLikeTestFile(hit.match.file_path) ? priority - kTestFilePenalty : priority;
            };
            std::stable_sort(merged.begin(), merged.end(), [&](const KeywordHit& a, const KeywordHit& b) {
                return hit_priority(a) > hit_priority(b);
            });
            if (merged.size() > options_.max_keyword_matches) {
                merged.resize(options_.max_keyword_matches);
                if (truncation != nullptr) {
                    truncation->keyword = true;
                }
            }
            // A hit inside a definition whose whole body already went in
            // is redundant; a hit elsewhere in a matched file (past a body
            // cut, or in a function no symbol matched) is not -- that is
            // the "answer buried in a huge function" case, see
            // kKeywordWindowLines.
            const auto already_visible = [&](const KeywordMatch& match) {
                return std::any_of(covered.begin(), covered.end(), [&](const CoveredRange& range) {
                    return range.file_path == match.file_path &&
                           match.line + kKeywordWindowLines >= range.first_line &&
                           match.line - kKeywordWindowLines <= range.last_line;
                });
            };
            std::vector<KeywordHit> kept;
            for (auto& hit : merged) {
                if (!passes_firewall(hit.match.file_path) || already_visible(hit.match)) {
                    continue;
                }
                kept.push_back(std::move(hit));
            }
            // Group by file, in line order, so neighbouring hits merge.
            std::stable_sort(kept.begin(), kept.end(), [](const KeywordHit& a, const KeywordHit& b) {
                return a.match.file_path != b.match.file_path ? a.match.file_path < b.match.file_path
                                                              : a.match.line < b.match.line;
            });
            for (std::size_t i = 0; i < kept.size();) {
                const auto& first = kept[i].match;
                int window_first = std::max(1, first.line - kKeywordWindowLines);
                int window_last = first.line + kKeywordWindowLines;
                int best_rank = HitRank(kept[i]);
                std::size_t j = i + 1;
                while (j < kept.size() && kept[j].match.file_path == first.file_path &&
                       kept[j].match.line - kKeywordWindowLines <= window_last) {
                    window_last = kept[j].match.line + kKeywordWindowLines;
                    best_rank = std::max(best_rank, HitRank(kept[j]));
                    ++j;
                }

                // A hit in a file small enough to send whole brings the
                // file instead of a thirteen-line window around the line.
                // Same trade the Symbol tier already makes: measured
                // 2026-09-24, the file that answered one task was pointed
                // at by exactly such a hit (5 KB, and the model spent a
                // turn fetching a range of it), and a round trip costs far
                // more than the file.
                ContextItem item;
                item.id = "keyword:" + first.file_path + ":" + std::to_string(first.line);
                item.source = ContextSourceKind::Custom;
                int priority = KeywordPriority(best_rank) + affinity_of(first.file_path);
                if (LooksLikeTestFile(first.file_path)) {
                    priority = std::max(0, priority - kTestFilePenalty);
                }
                item.priority = bias.Apply(priority, first.file_path);
                item.depends_on = {first.file_path};

                // ...but only a hit strong enough to survive the Relative
                // cutoff on its own. A weaker one is kept, if at all, only
                // as a call site (see that block), and a call site is the
                // window around the call, not the caller's whole file.
                // Measured 2026-09-29 (GameEngine, bench/payload.mjs): two
                // such hits brought ImGuiManager.cpp and Mesh.cpp whole,
                // 9.4 KB of a 20 KB response to "how the D3D12 device and
                // swap chain are created".
                int best_so_far = item.priority;
                for (const auto& existing : items) {
                    best_so_far = std::max(best_so_far, existing.priority);
                }
                std::optional<std::string> whole_content;
                if (item.priority >= best_so_far - kRelativePriorityCutoff &&
                    whole_file_for(first.file_path) != nullptr) {
                    whole_content = FileContent(first.file_path, snapshot->contents, metadata_by_path);
                }

                const auto content = FileContent(first.file_path, snapshot->contents, metadata_by_path);
                const auto snippet = content.has_value()
                                         ? SliceLines(*content, window_first, window_last, kMaxBodyLines)
                                         : LineSlice{};
                if (whole_content.has_value()) {
                    // Small enough to send whole, so the window becomes the
                    // file: the same trade the Symbol tier already makes.
                    // Measured 2026-09-24, the file that answered one task
                    // was pointed at by exactly such a hit (5 KB) and the
                    // model still spent a turn fetching a range of it.
                    item.compression = CompressionLevel::Raw;
                    item.content = first.file_path + "\n" + *std::move(whole_content);
                    covered.push_back({first.file_path, 1, std::numeric_limits<int>::max()});
                } else if (snippet.text.empty()) {
                    item.compression = CompressionLevel::Reference;
                    item.content = first.file_path + ":" + std::to_string(first.line) + ": " + first.text;
                } else {
                    item.compression = CompressionLevel::Summary;
                    item.content = first.file_path + ":" + std::to_string(window_first) + "-" +
                                   std::to_string(window_last) + "\n" + snippet.text;
                }
                item.estimated_tokens = EstimateTokens(item.content);
                add_item(std::move(item));
                i = j;
            }
        }
    }

    // Backend Context Provider retrieval (see class comment) — every
    // registered Backend gets a chance to contribute, native or Plugin
    // alike; IBackend::ProvideContext()'s own default returns nothing,
    // so a Backend that doesn't override it costs one empty-vector call.
    // Ranked below every intent-matched tier above (Keyword's floor is
    // 15): the built-in Backends' items are intent-independent bulk
    // (ProjectRulesBackend sends the whole CLAUDE.md on every call at a
    // constant 60; GitBackend sends up to 4000-char commit diffs at 50).
    // Measured on this repository (2026-09-18), those two alone took 75%
    // of a 2000-token response budget while the symbols that actually
    // answered the intent were left as stubs. They still fill whatever
    // budget the matched code leaves over.
    if (options_.backend_registry != nullptr) {
        for (const auto& backend : options_.backend_registry->All()) {
            for (auto& item : backend->ProvideContext(intent)) {
                if (!passes_firewall(item.id)) {
                    continue;
                }
                item.priority = std::min(item.priority, kBackendPriorityCap);
                add_item(std::move(item));
            }
        }
    }

    // Relative cutoff: how much weaker than the best match an item may be
    // and still be worth sending.
    //
    // The response budget is an absolute cap, so a retrieval that already
    // knows the answer fills the rest of it with whatever ranked next --
    // and the caller pays for that filler on every later turn. Measured
    // across all 15 bench tasks (2026-09-23), the core side cost roughly
    // the same (30-50k tokens) whether the question was easy or hard,
    // while the grep-and-read baseline it is compared against cost 26k on
    // an easy one and 64k on a hard one. That is the whole gap: on a task
    // with an obvious answer, a full budget of context is waste.
    //
    // Only the fallback tier is cut. Keyword retrieval exists for "text
    // the symbol and file names missed" (see its own block above), and
    // Backend items are intent-independent bulk -- when symbol/file
    // matching did NOT miss, both are filler. Symbol, File and
    // Dependency items are never dropped here: they are the answer and
    // its map. A phrase hit, which KeywordPriority deliberately lets
    // reach 60, survives this cut for the same reason it was promoted --
    // it is precise evidence, not fallback breadth.
    //
    // A keyword hit in a file the response does NOT otherwise contain is
    // exempt: that is where the matched code is USED, and it is the
    // answer to half the questions asked here ("...and who calls it?").
    // Cutting those cost three extra turns on one task (measured
    // 2026-09-23: the model went looking for the call site itself, in
    // main.cpp, which one surviving keyword hit would have handed it).
    // A hit in a file already included is genuinely redundant.
    if (!items.empty()) {
        std::unordered_set<std::string> represented = matched_files;
        for (const auto& item : items) {
            if (item.source == ContextSourceKind::File) {
                represented.insert(item.id);
            }
        }
        const auto best = std::max_element(items.begin(), items.end(),
                                            [](const ContextItem& a, const ContextItem& b) {
                                                return a.priority < b.priority;
                                            })
                              ->priority;
        const int floor = best - kRelativePriorityCutoff;
        std::size_t call_sites_kept = 0;
        const auto drop = [&](const ContextItem& item) {
            if (item.priority >= floor) {
                return false;
            }
            if (item.source == ContextSourceKind::GitDiff) {
                return true;
            }
            if (item.source != ContextSourceKind::Custom) {
                return false;
            }
            const bool elsewhere =
                !item.depends_on.empty() && represented.count(item.depends_on.front()) == 0;
            // Capped: a few call sites are the wiring, eight of them are
            // a wall of maybes the caller carries for the rest of the
            // session (measured 2026-09-23 on the suite's worst task).
            return !elsewhere || ++call_sites_kept > kMaxExemptedCallSites;
        };
        items.erase(std::remove_if(items.begin(), items.end(), drop), items.end());

    }

    return items;
}

} // namespace aistudio::core
