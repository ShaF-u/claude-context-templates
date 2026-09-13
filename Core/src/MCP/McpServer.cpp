#include "Core/MCP/McpServer.hpp"

#include "Core/Context/ContextBudget.hpp"
#include "Core/Context/ContextCompressor.hpp"
#include "Core/Context/ContextSelector.hpp"
#include "Core/Git/GitBackend.hpp"
#include "Core/Logging/Logger.hpp"
#include "Core/Search/KeywordSearch.hpp"
#include "Core/Search/SymbolSearch.hpp"
#include "Core/Util/TextRange.hpp"
#include "Core/Util/Utf8.hpp"

// Vendored header-only third-party library (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since this header isn't ours to fix —
// same pattern as Core/src/API/ApiServer.cpp.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <any>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace aistudio::core {

namespace {

using Json = nlohmann::json;

Json MakeResult(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json MakeError(const Json& id, int code, const std::string& message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", code}, {"message", message}}}};
}

// MCP tool results wrap everything in a `content` array regardless of
// success/failure; `isError` (absent = false) is how a failed tool call
// is distinguished from a JSON-RPC protocol-level error — the calling
// model sees it as a normal response it can react to, not a transport
// fault.
Json TextContent(const std::string& text) {
    return Json{{"content", Json::array({Json{{"type", "text"}, {"text", text}}})}};
}

Json ToolError(const std::string& message) {
    Json result = TextContent(message);
    result["isError"] = true;
    return result;
}

// ResponseItemCost() must measure exactly the shape that gets sent, so
// both go through this one function.
Json ContextItemJson(const ContextItem& item) {
    return Json{
        {"id", item.id},
        {"source", ToString(item.source)},
        {"compression", ToString(item.compression)},
        {"priority", item.priority},
        {"estimated_tokens", item.estimated_tokens},
        {"content", item.content},
    };
}

// Content-free stand-in for an item the budget couldn't fit; the content
// comes from a follow-up context_fetch.
Json OmittedContextItemJson(const ContextItem& item) {
    return Json{
        {"id", item.id},
        {"source", ToString(item.source)},
        {"compression", ToString(item.compression)},
        {"priority", item.priority},
        {"estimated_tokens", item.estimated_tokens},
        {"reason", "budget"},
    };
}

// docs/ROADMAP.md CE-4 sent ledger: content-free stand-in for an item
// whose content exactly matches what this session already sent for that
// id -- context_fetch is the always-available way to read it again.
Json UnchangedContextItemJson(const ContextItem& item, std::int64_t sent_at_ordinal) {
    return Json{
        {"id", item.id},
        {"unchanged_since", sent_at_ordinal},
        {"hint", "content unchanged; use context_fetch to re-read"},
    };
}

// Applied to every item about to be sent WITH its content, when the sent
// ledger is enabled: replaces it with UnchangedContextItemJson() if this
// session already sent this exact (id, content) pair, otherwise records
// it as sent and returns ContextItemJson() unchanged.
Json ContextItemJsonThroughLedger(const ContextItem& item, const McpServerOptions& options) {
    if (options.suppress_resent_content && options.sent_ledger != nullptr) {
        if (const auto check = options.sent_ledger->Check(item.id, item.content); check.unchanged) {
            return UnchangedContextItemJson(item, check.sent_at_ordinal);
        }
        options.sent_ledger->RecordSent(item.id, item.content);
    }
    return ContextItemJson(item);
}

// The serialized entry, not just content: charging estimated_tokens would
// undercount every item by its JSON envelope and escaping.
std::int64_t ResponseItemCost(const ContextItem& item) {
    return EstimateTokens(ContextItemJson(item).dump());
}

std::size_t ReadMaxResults(const Json& arguments, std::size_t fallback) {
    // Deliberately is_number_integer() rather than is_number_unsigned():
    // nlohmann only tags a parsed number as "unsigned" once it's too big
    // for int64_t, so an ordinary value like 5 or 50 parses as a signed
    // number_integer and would never match is_number_unsigned().
    if (arguments.contains("max_results") && arguments["max_results"].is_number_integer()) {
        const auto value = arguments["max_results"].get<std::int64_t>();
        if (value > 0) {
            return static_cast<std::size_t>(value);
        }
    }
    return fallback;
}

// nullopt if the id escapes project_root, doesn't exist, or isn't a
// regular file -- so a server without a `firewall` still can't be walked
// out of its project directory.
std::optional<std::filesystem::path> ResolveProjectFile(const std::string& project_root, const std::string& id) {
    if (project_root.empty() || id.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    // Utf8ToPath(), not std::filesystem::path(narrow_string) directly --
    // fs::path's own narrow-string constructor round-trips through the OS
    // system ANSI code page (CP_ACP) on Windows, NOT UTF-8, which `id`
    // (a connected AI's own tool-call argument) always is in this
    // codebase. See docs/ROADMAP.md "重大バグ発見・修正..." and
    // Utf8ToPath's own doc comment (Core/Util/Utf8.hpp).
    const auto root = std::filesystem::weakly_canonical(Utf8ToPath(project_root), ec);
    if (ec) {
        return std::nullopt;
    }
    const auto target = std::filesystem::weakly_canonical(root / Utf8ToPath(id), ec);
    if (ec) {
        return std::nullopt;
    }
    const auto relative = PathToUtf8Generic(target.lexically_relative(root));
    if (relative.empty() || relative == ".." || relative.rfind("../", 0) == 0) {
        return std::nullopt;
    }
    if (!std::filesystem::is_regular_file(target, ec)) {
        return std::nullopt;
    }
    return target;
}

// Falls back to CP932 (Shift-JIS) decoding when the raw bytes aren't
// valid UTF-8 -- see Cp932ToUtf8's own doc comment for why (this
// project's own host repo, GameEngine, saves most of Engine/Source that
// way). Every context_fetch call site below checks IsValidUtf8(*content)
// itself right after calling this, so a file that's neither valid UTF-8
// nor valid CP932 (genuinely binary) still reaches that check unchanged
// and gets rejected exactly as before this fallback existed.
std::optional<std::string> ReadFileContent(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    if (!IsValidUtf8(content)) {
        if (auto converted = Cp932ToUtf8(content); converted.has_value()) {
            return converted;
        }
    }
    return content;
}

// docs/ROADMAP.md CE-5's "context_fetch can't recover Symbol/Dependency/
// GitDiff/Custom ids" finding: these four helpers parse each source's own
// id convention (all defined by their respective *ContextSource.cpp/
// GitBackend.cpp/ContextRetriever.cpp construction sites) back apart. The
// caller must supply which one applies via context_fetch's own `source`
// argument -- id SHAPE ALONE is not a safe way to guess a kind (a Symbol
// id for an `operator->` overload contains both a lone ':' and a literal
// "->", the exact ambiguity docs/ROADMAP.md's Context Restore work had
// already hit once before and ruled out solving by inference).

// Symbol ids are "<file_path>:<symbol_name>" (SymbolContextSource.cpp).
// project-relative file_path never itself contains ':', so the first ':'
// NOT immediately followed by another ':' (i.e. not part of a "::" inside
// the qualified symbol_name) is the real separator.
std::optional<std::pair<std::string, std::string>> SplitSymbolId(const std::string& id) {
    for (std::size_t i = 0; i < id.size(); ++i) {
        if (id[i] == ':' && (i + 1 >= id.size() || id[i + 1] != ':')) {
            return std::make_pair(id.substr(0, i), id.substr(i + 1));
        }
    }
    return std::nullopt;
}

// Dependency ids are "<file_path>-><related_path>" (DependencyContextSource.cpp).
std::optional<std::pair<std::string, std::string>> SplitDependencyId(const std::string& id) {
    const auto pos = id.find("->");
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    return std::make_pair(id.substr(0, pos), id.substr(pos + 2));
}

// GitDiff ids are "git/commit/<sha>" (GitBackend.cpp's ProvideContext()).
// Same hex-only shape GitBackend.hpp's own git.show validates, checked
// again here since this is a separate entry point into the same backend.
std::optional<std::string> ParseGitCommitId(const std::string& id) {
    static constexpr std::string_view kPrefix = "git/commit/";
    if (id.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    const auto sha = id.substr(kPrefix.size());
    if (sha.size() < 4 || sha.size() > 40) {
        return std::nullopt;
    }
    if (!std::all_of(sha.begin(), sha.end(), [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return std::nullopt;
    }
    return sha;
}

// Custom ids following the "keyword:<file_path>:<line>" convention
// (ContextRetriever.cpp's Keyword retrieval; Core/Context/ContextRestorer.cpp's
// kKeywordIdPrefix documents the same convention for the restore path).
// Other Custom ids (e.g. ProjectRulesBackend's "rules/project", or any
// Plugin/native Backend's own scheme) aren't in this convention and stay
// unsupported -- Custom is backend-defined, not a single parseable shape.
std::optional<std::pair<std::string, int>> ParseKeywordId(const std::string& id) {
    static constexpr std::string_view kPrefix = "keyword:";
    if (id.rfind(kPrefix, 0) != 0) {
        return std::nullopt;
    }
    const auto rest = id.substr(kPrefix.size());
    const auto sep = rest.rfind(':');
    if (sep == std::string::npos) {
        return std::nullopt;
    }
    const auto path = rest.substr(0, sep);
    const auto line_str = rest.substr(sep + 1);
    if (line_str.empty() || !std::all_of(line_str.begin(), line_str.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        return std::nullopt;
    }
    const int line = std::stoi(line_str);
    if (line < 1) {
        return std::nullopt;
    }
    return std::make_pair(path, line);
}

Json AstNodeToJson(const AstNode& node) {
    Json children = Json::array();
    for (const auto& child : node.children) {
        children.push_back(AstNodeToJson(child));
    }
    Json result = Json{
        {"kind", node.kind},
        {"start_line", node.start_line},
        {"end_line", node.end_line},
        {"children", children},
    };
    if (!node.text.empty()) {
        result["text"] = node.text;
    }
    return result;
}

Json ToolsList(const McpServerOptions& options) {
    Json tools = Json::array();

    if (options.symbol_index != nullptr) {
        tools.push_back(Json{
            {"name", "symbol_search"},
            {"description", "Ranked lookup of Class/Struct/Function/Namespace/Variable symbols by name or "
                             "partial name across the indexed project. Prefer this over reading whole files "
                             "when you already know roughly what symbol you're looking for."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"query", Json{{"type", "string"}, {"description", "Symbol name or partial name."}}},
                      {"max_results", Json{{"type", "integer"}, {"minimum", 1}}},
                  }},
                 {"required", Json::array({"query"})},
             }},
        });
    }

    if (!options.project_root.empty()) {
        tools.push_back(Json{
            {"name", "keyword_search"},
            {"description", "Case-insensitive full-text substring search across the project's source files, "
                             "line by line. Use symbol_search first for known identifiers; use this for free "
                             "text, comments, or strings."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"query", Json{{"type", "string"}, {"description", "Text to search for."}}},
                      {"max_results", Json{{"type", "integer"}, {"minimum", 1}}},
                  }},
                 {"required", Json::array({"query"})},
             }},
        });
    }

    if (options.context_retriever != nullptr) {
        tools.push_back(Json{
            {"name", "context_retrieve"},
            {"description", "Intent-driven retrieval combining symbol, file, dependency, and keyword matches, "
                             "plus whatever registered Backends contribute for this intent (e.g. File/Git "
                             "Provider content), into a single ranked result (docs/MASTER_SPEC.md Context "
                             "Engine). Given a free-text description of a task, returns the relevant "
                             "symbols/files/dependencies/commits instead of requiring you to guess which whole "
                             "files to read. When a response token budget is configured, lower-priority items "
                             "that don't fit are returned content-free in an \"omitted\" array (with a "
                             "\"budget\" summary) instead of being dropped — pass such an id to context_fetch "
                             "to read it."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"intent", Json{{"type", "string"},
                                       {"description", "Free-text description of what you're looking for, "
                                                        "e.g. \"player attack logic\"."}}},
                  }},
                 {"required", Json::array({"intent"})},
             }},
        });
    }

    if (!options.project_root.empty()) {
        tools.push_back(Json{
            {"name", "context_fetch"},
            {"description", "Reads one project file, whole or by line range — the explicit \"give me the full "
                             "text\" step of the retrieval ladder (docs/MASTER_SPEC.md #99). Use it to follow "
                             "up on a context_retrieve item that came back omitted or summarized, rather than "
                             "asking for whole files up front."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"id", Json{{"type", "string"},
                                   {"description", "Project-relative file path — the id a File-source "
                                                    "context_retrieve item carries."}}},
                      {"mode", Json{{"type", "string"},
                                     {"enum", Json::array({"full", "range"})},
                                     {"description", "\"full\" (default): the whole file. \"range\": only "
                                                      "start_line..end_line."}}},
                      {"start_line", Json{{"type", "integer"}, {"minimum", 1},
                                           {"description", "1-based, inclusive. Required for mode \"range\"."}}},
                      {"end_line", Json{{"type", "integer"}, {"minimum", 1},
                                         {"description", "1-based, inclusive. Required for mode \"range\"."}}},
                      {"max_tokens", Json{{"type", "integer"}, {"minimum", 1},
                                           {"description", "Optional cap; a longer result comes back as a "
                                                            "head/tail excerpt with an omission marker."}}},
                  }},
                 {"required", Json::array({"id"})},
             }},
        });
    }

    if (options.include_graph != nullptr) {
        tools.push_back(Json{
            {"name", "include_graph"},
            {"description", "Looks up #include relationships for one file: what it includes, or what includes "
                             "it. Useful for understanding compile-time dependencies before changing a header."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"file_path", Json{{"type", "string"}, {"description", "Project-relative path of the file."}}},
                      {"direction", Json{{"type", "string"},
                                          {"enum", Json::array({"includes", "included_by"})},
                                          {"description", "\"includes\" (default): files this file includes. "
                                                           "\"included_by\": files that include this file."}}},
                  }},
                 {"required", Json::array({"file_path"})},
             }},
        });
    }

    if (options.call_graph != nullptr) {
        tools.push_back(Json{
            {"name", "call_graph"},
            {"description", "Looks up call relationships for one function/method by its qualified name: what it "
                             "calls, or (heuristically, by name -- no type resolution) what calls it. Use "
                             "before renaming or changing a function's behavior to see what else might be "
                             "affected."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"symbol_name", Json{{"type", "string"},
                                            {"description", "Qualified function/method name, e.g. "
                                                             "\"ContextSelector::Select\"."}}},
                      {"direction", Json{{"type", "string"},
                                          {"enum", Json::array({"callees", "callers"})},
                                          {"description", "\"callers\" (default): call sites that call this "
                                                           "symbol. \"callees\": calls made from inside this "
                                                           "symbol's body."}}},
                  }},
                 {"required", Json::array({"symbol_name"})},
             }},
        });
    }

    if (options.inheritance_graph != nullptr) {
        tools.push_back(Json{
            {"name", "inheritance_graph"},
            {"description", "Looks up class inheritance relationships by class name: direct base classes, or "
                             "direct subclasses."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"class_name", Json{{"type", "string"},
                                           {"description", "Plain class/struct name (no template arguments)."}}},
                      {"direction", Json{{"type", "string"},
                                          {"enum", Json::array({"bases", "derived"})},
                                          {"description", "\"derived\" (default): direct subclasses of this "
                                                           "class. \"bases\": this class's own direct base "
                                                           "classes."}}},
                  }},
                 {"required", Json::array({"class_name"})},
             }},
        });
    }

    if (options.reference_graph != nullptr) {
        tools.push_back(Json{
            {"name", "reference_graph"},
            {"description", "Finds every place a type name is referenced (declared type, parameter/return "
                             "type, base class, template argument, cast target) across the project -- not just "
                             "its definition."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"type_name", Json{{"type", "string"}, {"description", "Plain type name to search for."}}},
                  }},
                 {"required", Json::array({"type_name"})},
             }},
        });
    }

    if (options.ast_index != nullptr) {
        tools.push_back(Json{
            {"name", "ast_tree"},
            {"description", "Returns one file's full parse tree (named nodes only -- declarations, statements, "
                             "expressions, identifiers, literals, not punctuation) as nested "
                             "kind/start_line/end_line/children/text objects. Prefer symbol_search for a known "
                             "declaration's signature; use this when you need the actual structure (e.g. to find "
                             "where to insert a new member, or to see a function body's statement layout)."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"file_path", Json{{"type", "string"}, {"description", "Project-relative path of the file."}}},
                  }},
                 {"required", Json::array({"file_path"})},
             }},
        });
    }

    if (options.impact_analyzer != nullptr) {
        tools.push_back(Json{
            {"name", "impact_analysis"},
            {"description", "Given a file, reports every file that transitively includes it and every "
                             "function/method it defines together with (heuristically matched) call sites "
                             "elsewhere -- a starting estimate of \"what could this change affect?\" before "
                             "editing a file."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"file_path", Json{{"type", "string"},
                                          {"description", "Project-relative path of the file to analyze."}}},
                  }},
                 {"required", Json::array({"file_path"})},
             }},
        });
    }

    if (options.impact_analyzer != nullptr && options.backend_registry != nullptr) {
        tools.push_back(Json{
            {"name", "changed_impact_analysis"},
            {"description", "Reports every file, symbol, and downstream caller touched by the CURRENT "
                             "uncommitted changes (staged and unstaged combined) -- the live counterpart to "
                             "impact_analysis, answering \"what does my in-progress edit affect?\" without "
                             "needing to name a file. Only sees changes to already-tracked files; brand new "
                             "untracked files aren't visible yet."},
            {"inputSchema", Json{{"type", "object"}, {"properties", Json::object()}}},
        });
    }

    if (options.impact_analyzer != nullptr && options.backend_registry != nullptr) {
        tools.push_back(Json{
            {"name", "branch_impact_analysis"},
            {"description", "Reports every file, symbol, and downstream caller changed on the CURRENT branch "
                             "since it diverged from the given base branch (staged, unstaged, and already "
                             "committed changes all combined) -- the whole-branch/pull-request counterpart to "
                             "changed_impact_analysis, answering \"what does my whole branch change?\" before "
                             "opening or updating a PR."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"base_branch", Json{{"type", "string"},
                                            {"description", "Branch this one diverged from, e.g. \"Develop\" or "
                                                             "\"main\"."}}},
                  }},
                 {"required", Json::array({"base_branch"})},
             }},
        });
    }

    if (options.backend_registry != nullptr) {
        tools.push_back(Json{
            {"name", "project_rules"},
            {"description", "Returns this project's standing conventions/rules (docs/MASTER_SPEC.md #25) -- "
                             "coding style, architectural constraints, git workflow, etc -- so they don't need to "
                             "be re-explained. Call this once near the start of a task rather than guessing "
                             "project conventions."},
            {"inputSchema", Json{{"type", "object"}, {"properties", Json::object()}}},
        });
        tools.push_back(Json{
            {"name", "similar_change_search"},
            {"description", "Finds past commits that changed the given symbol/identifier before (pickaxe "
                             "search over commit content, not just messages), each with its full diff -- "
                             "\"how has this specific piece of code changed before?\" Useful before modifying a "
                             "function to see its own change history."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"symbol_name", Json{{"type", "string"},
                                            {"description", "Symbol/identifier name to search commit history for."}}},
                  }},
                 {"required", Json::array({"symbol_name"})},
             }},
        });
    }

    if (options.backend_registry != nullptr && options.enable_git_write_commands) {
        tools.push_back(Json{
            {"name", "git_commit"},
            {"description", "Stages ALL current working-tree changes (`git add -A`) and commits them with the "
                             "given message. There is no partial-staging control -- everything changed gets "
                             "committed together."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"message", Json{{"type", "string"}, {"description", "Commit message. Must be non-empty."}}},
                  }},
                 {"required", Json::array({"message"})},
             }},
        });
        tools.push_back(Json{
            {"name", "git_branch"},
            {"description", "Creates a new local branch pointing at the current HEAD. Does NOT switch to it -- "
                             "use git_checkout for that."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"name", Json{{"type", "string"}, {"description", "New branch name. Must be non-empty."}}},
                  }},
                 {"required", Json::array({"name"})},
             }},
        });
        tools.push_back(Json{
            {"name", "git_stash"},
            {"description", "Stashes all current working-tree changes (`git stash push`), returning the working "
                             "tree to a clean state."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"message", Json{{"type", "string"}, {"description", "Optional stash message."}}},
                  }},
             }},
        });
        tools.push_back(Json{
            {"name", "git_checkout"},
            {"description", "Switches to an EXISTING local branch (`git checkout <branch>` -- never creates one, "
                             "use git_branch first; never forces, so git itself refuses if uncommitted changes "
                             "would be overwritten)."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"branch", Json{{"type", "string"}, {"description", "Existing branch name. Must be non-empty."}}},
                  }},
                 {"required", Json::array({"branch"})},
             }},
        });
        tools.push_back(Json{
            {"name", "git_tag"},
            {"description", "Creates a lightweight tag pointing at the current HEAD (`git tag <name>` -- no "
                             "annotation/message, and never moves or deletes an existing tag)."},
            {"inputSchema",
             Json{
                 {"type", "object"},
                 {"properties",
                  Json{
                      {"name", Json{{"type", "string"}, {"description", "New tag name. Must be non-empty."}}},
                  }},
                 {"required", Json::array({"name"})},
             }},
        });
    }

    if (options.editor_state_store != nullptr) {
        tools.push_back(Json{
            {"name", "active_document"},
            {"description", "Reports which file (if any) is currently open/focused in a connected IDE extension "
                             "(Tools/vscode-extension today; any future Visual Studio/Rider counterpart reports "
                             "through the same mechanism), and its current text selection, if any. "
                             "active_document_path/selection are both null when no IDE extension is currently "
                             "reporting state -- that's the normal condition when no IDE is connected, not an "
                             "error. Use this to bias where you look next toward what the user is actually "
                             "looking at right now."},
            {"inputSchema", Json{{"type", "object"}, {"properties", Json::object()}}},
        });
    }

    return tools;
}

Json CallTool(const McpServerOptions& options, const std::string& name, const Json& arguments) {
    if (name == "symbol_search") {
        if (options.symbol_index == nullptr) {
            return ToolError("symbol_search is not available (no SymbolIndex configured)");
        }
        if (!arguments.contains("query") || !arguments["query"].is_string()) {
            return ToolError("symbol_search requires a string 'query' argument");
        }

        const SymbolSearch search;
        const auto matches =
            search.Search(*options.symbol_index, arguments["query"].get<std::string>(), ReadMaxResults(arguments, 50));

        Json list = Json::array();
        for (const auto& match : matches) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(match.symbol.file_path)) {
                continue;
            }
            list.push_back(Json{
                {"name", match.symbol.name},
                {"kind", ToString(match.symbol.kind)},
                {"file_path", match.symbol.file_path},
                {"line", match.symbol.line},
                {"signature", match.symbol.signature},
                {"score", match.score},
            });
        }
        return TextContent(Json{{"matches", list}}.dump());
    }

    if (name == "keyword_search") {
        if (options.project_root.empty()) {
            return ToolError("keyword_search is not available (no project_root configured)");
        }
        if (!arguments.contains("query") || !arguments["query"].is_string()) {
            return ToolError("keyword_search requires a string 'query' argument");
        }

        const KeywordSearch search;
        const auto result =
            search.Search(options.project_root, arguments["query"].get<std::string>(), ReadMaxResults(arguments, 200));
        if (!result) {
            return ToolError("keyword_search failed: " + result.Err().message);
        }

        Json list = Json::array();
        for (const auto& match : result.Value()) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(match.file_path)) {
                continue;
            }
            list.push_back(Json{
                {"file_path", match.file_path},
                {"line", match.line},
                {"text", match.text},
                {"score", match.score},
            });
        }
        return TextContent(Json{{"matches", list}}.dump());
    }

    if (name == "context_retrieve") {
        if (options.context_retriever == nullptr) {
            return ToolError("context_retrieve is not available (no ContextRetriever configured)");
        }
        if (!arguments.contains("intent") || !arguments["intent"].is_string()) {
            return ToolError("context_retrieve requires a string 'intent' argument");
        }

        const auto intent = arguments["intent"].get<std::string>();
        auto items = options.context_cache != nullptr
                          ? options.context_cache->GetOrRetrieve(*options.context_retriever, intent)
                          : options.context_retriever->Retrieve(intent);

        if (options.suppress_resent_content && options.sent_ledger != nullptr) {
            options.sent_ledger->BeginResponse();
        }

        // No budget: byte-for-byte the pre-existing response (when the
        // sent ledger is also off -- its default).
        if (options.context_response_budget_tokens <= 0) {
            Json list = Json::array();
            for (const auto& item : items) {
                list.push_back(ContextItemJsonThroughLedger(item, options));
            }
            return TextContent(Json{{"items", list}}.dump());
        }

        // AGENT.md #8's Compression + Budget Check, applied to the path an
        // external AI actually uses. ContextSelector also publishes a
        // "ContextAudit" event per decision, so MCP-path usage is now
        // visible to GET /api/context/usage for any subscriber.
        const ContextCompressor compressor;
        const ContextSelector selector;
        auto selection = selector.SelectWithCompression(std::move(items),
                                                          ContextBudget(options.context_response_budget_tokens),
                                                          compressor, ContextItemCost(ResponseItemCost));

        // Sent-ledger suppression is applied after budget selection, not
        // folded into it: `used_tokens`/`budget` below still charge the
        // full item cost, so a caller can't be told it has more headroom
        // than a session without ledger history would have.
        Json list = Json::array();
        for (const auto& item : selection.included) {
            list.push_back(ContextItemJsonThroughLedger(item, options));
        }
        Json omitted = Json::array();
        for (const auto& item : selection.excluded) {
            omitted.push_back(OmittedContextItemJson(item));
        }

        AISTUDIO_LOG_INFO("Core.MCP", "context_retrieve: intent='" + intent + "' included=" +
                                            std::to_string(list.size()) + " omitted=" +
                                            std::to_string(omitted.size()) + " tokens=" +
                                            std::to_string(selection.used_tokens) + "/" +
                                            std::to_string(options.context_response_budget_tokens));

        return TextContent(Json{
            {"items", list},
            {"omitted", omitted},
            {"budget",
             Json{
                 {"max_tokens", options.context_response_budget_tokens},
                 {"used_tokens", selection.used_tokens},
                 {"included", list.size()},
                 {"omitted", omitted.size()},
                 // EstimateTokens()'s ~4-chars-per-token approximation,
                 // not a model tokenizer's count.
                 {"unit", "estimate"},
             }},
        }.dump());
    }

    if (name == "context_fetch") {
        if (options.project_root.empty()) {
            return ToolError("context_fetch is not available (no project_root configured)");
        }
        if (!arguments.contains("id") || !arguments["id"].is_string()) {
            return ToolError("context_fetch requires a string 'id' argument");
        }
        const auto id = arguments["id"].get<std::string>();
        // docs/ROADMAP.md CE-5: optional, mirrors the "source" field the
        // caller already received on the omitted item -- the caller is
        // never asked to guess a kind from id shape (see SplitSymbolId's
        // own comment on why that's unsafe), only to echo back what it
        // was already given. Omitted/"File" is the pre-existing v1
        // behavior below, byte-for-byte unchanged.
        const auto source = arguments.value("source", std::string("File"));

        if (source == "Symbol") {
            if (options.symbol_index == nullptr) {
                return ToolError("context_fetch: source \"Symbol\" is not available (no SymbolIndex configured)");
            }
            const auto split = SplitSymbolId(id);
            if (!split) {
                return ToolError("context_fetch: '" + id + "' is not a recognized Symbol id (expected "
                                                              "\"<file_path>:<symbol_name>\")");
            }
            const auto& [file_path, symbol_name] = *split;
            if (options.firewall != nullptr && !options.firewall->IsAllowed(file_path)) {
                return ToolError("context_fetch: '" + file_path + "' is outside the allowed project scope");
            }
            const auto matches = options.symbol_index->FindByName(symbol_name);
            const auto match_it = std::find_if(matches.begin(), matches.end(), [&](const Symbol& s) {
                return s.file_path == file_path && s.name == symbol_name;
            });
            if (match_it == matches.end()) {
                return ToolError("context_fetch: '" + id + "' does not match a known symbol");
            }
            const auto resolved = ResolveProjectFile(options.project_root, file_path);
            if (!resolved) {
                return ToolError("context_fetch: '" + file_path + "' does not resolve to a readable file");
            }
            const auto content = ReadFileContent(*resolved);
            if (!content || !IsValidUtf8(*content)) {
                return ToolError("context_fetch: failed to read '" + file_path + "' as UTF-8 text");
            }
            const int total_lines = CountLines(*content);
            constexpr int kSymbolContextLines = 10;
            const int start_line = std::max(1, match_it->line - kSymbolContextLines);
            const int end_line = std::min(total_lines, match_it->line + kSymbolContextLines);

            ContextItem item;
            item.id = id;
            item.source = ContextSourceKind::Symbol;
            item.compression = CompressionLevel::Summary;
            item.content = ExtractLines(*content, start_line, end_line);
            item.estimated_tokens = EstimateTokens(item.content);

            bool compressed = false;
            if (arguments.contains("max_tokens") && arguments["max_tokens"].is_number_integer()) {
                const auto max_tokens = arguments["max_tokens"].get<std::int64_t>();
                if (max_tokens > 0 && item.estimated_tokens > max_tokens) {
                    const ContextCompressor compressor;
                    item = compressor.Compress(std::move(item), max_tokens);
                    compressed = true;
                }
            }

            return TextContent(Json{
                {"id", id},
                {"source", "Symbol"},
                {"resolved_file", file_path},
                {"symbol_line", match_it->line},
                {"start_line", start_line},
                {"end_line", end_line},
                {"total_lines", total_lines},
                {"compression", ToString(item.compression)},
                {"compressed", compressed},
                {"estimated_tokens", item.estimated_tokens},
                {"content", item.content},
            }.dump());
        }

        if (source == "Dependency") {
            const auto split = SplitDependencyId(id);
            if (!split) {
                return ToolError("context_fetch: '" + id + "' is not a recognized Dependency id (expected "
                                                              "\"<file_path>-><related_path>\")");
            }
            const auto& [file_path, related_path] = *split;
            if (options.firewall != nullptr &&
                (!options.firewall->IsAllowed(file_path) || !options.firewall->IsAllowed(related_path))) {
                return ToolError("context_fetch: '" + id + "' is outside the allowed project scope");
            }
            // No file read needed -- the full content a Dependency item
            // can ever carry is this one synthesized line (see
            // DependencyContextSource.cpp's own MakeItem()), already
            // reconstructable from the id alone.
            ContextItem item;
            item.id = id;
            item.source = ContextSourceKind::Dependency;
            item.compression = CompressionLevel::Reference;
            item.content = file_path + " includes " + related_path;
            item.estimated_tokens = EstimateTokens(item.content);

            return TextContent(Json{
                {"id", id},
                {"source", "Dependency"},
                {"compression", ToString(item.compression)},
                {"compressed", false},
                {"estimated_tokens", item.estimated_tokens},
                {"content", item.content},
            }.dump());
        }

        if (source == "GitDiff") {
            if (options.backend_registry == nullptr) {
                return ToolError("context_fetch: source \"GitDiff\" is not available (no BackendRegistry configured)");
            }
            const auto sha = ParseGitCommitId(id);
            if (!sha) {
                return ToolError("context_fetch: '" + id +
                                  "' is not a recognized GitDiff id (expected \"git/commit/<sha>\")");
            }
            Query query;
            query.backend_id = "core.git";
            query.name = "git.show";
            query.parameters = std::any(*sha);
            const auto result = options.backend_registry->RunQuery(query);
            if (!result) {
                return ToolError("context_fetch: git.show failed for '" + *sha + "': " + result.Err().message);
            }
            auto diff_text = std::any_cast<std::string>(result.Value());
            if (!IsValidUtf8(diff_text)) {
                return ToolError("context_fetch: '" + id + "' is not valid UTF-8 text");
            }

            ContextItem item;
            item.id = id;
            item.source = ContextSourceKind::GitDiff;
            item.compression = CompressionLevel::Raw;
            item.content = std::move(diff_text);
            item.estimated_tokens = EstimateTokens(item.content);

            bool compressed = false;
            if (arguments.contains("max_tokens") && arguments["max_tokens"].is_number_integer()) {
                const auto max_tokens = arguments["max_tokens"].get<std::int64_t>();
                if (max_tokens > 0 && item.estimated_tokens > max_tokens) {
                    const ContextCompressor compressor;
                    item = compressor.Compress(std::move(item), max_tokens);
                    compressed = true;
                }
            }

            return TextContent(Json{
                {"id", id},
                {"source", "GitDiff"},
                {"sha", *sha},
                {"compression", ToString(item.compression)},
                {"compressed", compressed},
                {"estimated_tokens", item.estimated_tokens},
                {"content", item.content},
            }.dump());
        }

        if (source == "Custom") {
            const auto parsed = ParseKeywordId(id);
            if (!parsed) {
                return ToolError("context_fetch: '" + id +
                                  "' is a Custom item not in the \"keyword:<file_path>:<line>\" convention -- "
                                  "this Backend-provided item cannot be re-fetched");
            }
            const auto& [file_path, line] = *parsed;
            if (options.firewall != nullptr && !options.firewall->IsAllowed(file_path)) {
                return ToolError("context_fetch: '" + file_path + "' is outside the allowed project scope");
            }
            const auto resolved = ResolveProjectFile(options.project_root, file_path);
            if (!resolved) {
                return ToolError("context_fetch: '" + file_path + "' does not resolve to a readable file");
            }
            const auto content = ReadFileContent(*resolved);
            if (!content || !IsValidUtf8(*content)) {
                return ToolError("context_fetch: failed to read '" + file_path + "' as UTF-8 text");
            }
            const int total_lines = CountLines(*content);
            constexpr int kKeywordContextLines = 10;
            const int start_line = std::max(1, line - kKeywordContextLines);
            const int end_line = std::min(total_lines, line + kKeywordContextLines);

            ContextItem item;
            item.id = id;
            item.source = ContextSourceKind::Custom;
            item.compression = CompressionLevel::Summary;
            item.content = ExtractLines(*content, start_line, end_line);
            item.estimated_tokens = EstimateTokens(item.content);

            bool compressed = false;
            if (arguments.contains("max_tokens") && arguments["max_tokens"].is_number_integer()) {
                const auto max_tokens = arguments["max_tokens"].get<std::int64_t>();
                if (max_tokens > 0 && item.estimated_tokens > max_tokens) {
                    const ContextCompressor compressor;
                    item = compressor.Compress(std::move(item), max_tokens);
                    compressed = true;
                }
            }

            return TextContent(Json{
                {"id", id},
                {"source", "Custom"},
                {"resolved_file", file_path},
                {"start_line", start_line},
                {"end_line", end_line},
                {"total_lines", total_lines},
                {"compression", ToString(item.compression)},
                {"compressed", compressed},
                {"estimated_tokens", item.estimated_tokens},
                {"content", item.content},
            }.dump());
        }

        if (source != "File") {
            return ToolError("context_fetch: unknown 'source' \"" + source +
                              "\" (expected \"File\", \"Symbol\", \"Dependency\", \"GitDiff\", or \"Custom\")");
        }

        if (options.firewall != nullptr && !options.firewall->IsAllowed(id)) {
            return ToolError("context_fetch: '" + id + "' is outside the allowed project scope");
        }

        const auto resolved = ResolveProjectFile(options.project_root, id);
        if (!resolved) {
            // Symbol/Keyword/Backend ids aren't file paths; erroring beats
            // guessing which file the caller meant.
            return ToolError("context_fetch: '" + id +
                              "' does not resolve to a readable file under the project root. v1 accepts a "
                              "project-relative file path (the id File-source items use).");
        }

        const auto content = ReadFileContent(*resolved);
        if (!content) {
            return ToolError("context_fetch: failed to read '" + id + "'");
        }
        if (!IsValidUtf8(*content)) {
            // A binary file's raw bytes can't become JSON string content
            // (nlohmann::json::dump() would throw) -- rejecting here beats
            // crashing the whole response the way FileScanner having no
            // text/binary distinction otherwise would.
            return ToolError("context_fetch: '" + id + "' is not valid UTF-8 text (binary file?)");
        }

        const auto mode = arguments.value("mode", std::string("full"));
        const int total_lines = CountLines(*content);

        ContextItem item;
        item.id = id;
        item.source = ContextSourceKind::File;
        int start_line = 1;
        int end_line = total_lines;

        if (mode == "range") {
            if (!arguments.contains("start_line") || !arguments["start_line"].is_number_integer() ||
                !arguments.contains("end_line") || !arguments["end_line"].is_number_integer()) {
                return ToolError("context_fetch: mode \"range\" requires integer 'start_line' and 'end_line' "
                                  "(1-based, inclusive)");
            }
            start_line = arguments["start_line"].get<int>();
            end_line = arguments["end_line"].get<int>();
            if (start_line < 1 || end_line < start_line) {
                return ToolError("context_fetch: 'start_line' must be >= 1 and 'end_line' >= 'start_line'");
            }
            item.content = ExtractLines(*content, start_line, end_line);
            item.compression = CompressionLevel::Summary;
            start_line = std::min(start_line, total_lines);
            end_line = std::min(end_line, total_lines);
        } else if (mode == "full") {
            item.content = *content;
            item.compression = CompressionLevel::Raw;
        } else {
            return ToolError("context_fetch: unknown mode '" + mode + "' (expected \"full\" or \"range\")");
        }
        item.estimated_tokens = EstimateTokens(item.content);

        bool compressed = false;
        if (arguments.contains("max_tokens") && arguments["max_tokens"].is_number_integer()) {
            const auto max_tokens = arguments["max_tokens"].get<std::int64_t>();
            if (max_tokens > 0 && item.estimated_tokens > max_tokens) {
                const ContextCompressor compressor;
                item = compressor.Compress(std::move(item), max_tokens);
                compressed = true;
            }
        }

        return TextContent(Json{
            {"id", id},
            {"mode", mode},
            {"start_line", start_line},
            {"end_line", end_line},
            {"total_lines", total_lines},
            {"compression", ToString(item.compression)},
            {"compressed", compressed},
            {"estimated_tokens", item.estimated_tokens},
            {"content", item.content},
        }.dump());
    }

    if (name == "include_graph") {
        if (options.include_graph == nullptr) {
            return ToolError("include_graph is not available (no IncludeGraph configured)");
        }
        if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
            return ToolError("include_graph requires a string 'file_path' argument");
        }
        const auto file_path = arguments["file_path"].get<std::string>();
        const auto direction = arguments.value("direction", std::string("includes"));
        const auto files = direction == "included_by" ? options.include_graph->IncludedBy(file_path)
                                                        : options.include_graph->Includes(file_path);

        Json list = Json::array();
        for (const auto& f : files) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(f)) {
                continue;
            }
            list.push_back(f);
        }
        return TextContent(Json{{"file_path", file_path}, {"direction", direction}, {"files", list}}.dump());
    }

    if (name == "call_graph") {
        if (options.call_graph == nullptr) {
            return ToolError("call_graph is not available (no CallGraph configured)");
        }
        if (!arguments.contains("symbol_name") || !arguments["symbol_name"].is_string()) {
            return ToolError("call_graph requires a string 'symbol_name' argument");
        }
        const auto symbol_name = arguments["symbol_name"].get<std::string>();
        const auto direction = arguments.value("direction", std::string("callers"));
        const auto edges = direction == "callees" ? options.call_graph->Callees(symbol_name)
                                                    : options.call_graph->Callers(symbol_name);

        Json list = Json::array();
        for (const auto& edge : edges) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(edge.caller_file)) {
                continue;
            }
            list.push_back(Json{
                {"caller_name", edge.caller_name},
                {"caller_file", edge.caller_file},
                {"line", edge.line},
                {"callee_text", edge.callee_text},
            });
        }
        return TextContent(Json{{"symbol_name", symbol_name}, {"direction", direction}, {"edges", list}}.dump());
    }

    if (name == "inheritance_graph") {
        if (options.inheritance_graph == nullptr) {
            return ToolError("inheritance_graph is not available (no InheritanceGraph configured)");
        }
        if (!arguments.contains("class_name") || !arguments["class_name"].is_string()) {
            return ToolError("inheritance_graph requires a string 'class_name' argument");
        }
        const auto class_name = arguments["class_name"].get<std::string>();
        const auto direction = arguments.value("direction", std::string("derived"));
        const auto edges = direction == "bases" ? options.inheritance_graph->Bases(class_name)
                                                  : options.inheritance_graph->Derived(class_name);

        Json list = Json::array();
        for (const auto& edge : edges) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(edge.derived_file)) {
                continue;
            }
            list.push_back(Json{
                {"derived_name", edge.derived_name},
                {"derived_file", edge.derived_file},
                {"line", edge.line},
                {"base_name", edge.base_name},
            });
        }
        return TextContent(Json{{"class_name", class_name}, {"direction", direction}, {"edges", list}}.dump());
    }

    if (name == "reference_graph") {
        if (options.reference_graph == nullptr) {
            return ToolError("reference_graph is not available (no ReferenceGraph configured)");
        }
        if (!arguments.contains("type_name") || !arguments["type_name"].is_string()) {
            return ToolError("reference_graph requires a string 'type_name' argument");
        }
        const auto type_name = arguments["type_name"].get<std::string>();
        const auto edges = options.reference_graph->References(type_name);

        Json list = Json::array();
        for (const auto& edge : edges) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(edge.referencing_file)) {
                continue;
            }
            list.push_back(Json{
                {"type_name", edge.type_name},
                {"referencing_file", edge.referencing_file},
                {"line", edge.line},
            });
        }
        return TextContent(Json{{"type_name", type_name}, {"references", list}}.dump());
    }

    if (name == "ast_tree") {
        if (options.ast_index == nullptr) {
            return ToolError("ast_tree is not available (no AstIndex configured)");
        }
        if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
            return ToolError("ast_tree requires a string 'file_path' argument");
        }
        const auto file_path = arguments["file_path"].get<std::string>();
        if (options.firewall != nullptr && !options.firewall->IsAllowed(file_path)) {
            return ToolError("ast_tree: file not accessible: " + file_path);
        }
        const auto tree = options.ast_index->Get(file_path);
        if (!tree.has_value()) {
            return ToolError("no AST indexed for file: " + file_path);
        }
        return TextContent(Json{{"file_path", file_path}, {"tree", AstNodeToJson(*tree)}}.dump());
    }

    if (name == "impact_analysis") {
        if (options.impact_analyzer == nullptr) {
            return ToolError("impact_analysis is not available (no ImpactAnalyzer configured)");
        }
        if (!arguments.contains("file_path") || !arguments["file_path"].is_string()) {
            return ToolError("impact_analysis requires a string 'file_path' argument");
        }
        const auto result = options.impact_analyzer->Analyze(arguments["file_path"].get<std::string>());

        Json affected_files = Json::array();
        for (const auto& f : result.affected_files) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(f)) {
                continue;
            }
            affected_files.push_back(f);
        }

        Json affected_symbols = Json::array();
        for (const auto& symbol : result.affected_symbols) {
            Json callers = Json::array();
            for (const auto& caller : symbol.callers) {
                if (options.firewall != nullptr && !options.firewall->IsAllowed(caller.caller_file)) {
                    continue;
                }
                callers.push_back(Json{
                    {"caller_name", caller.caller_name},
                    {"caller_file", caller.caller_file},
                    {"line", caller.line},
                    {"callee_text", caller.callee_text},
                });
            }
            affected_symbols.push_back(Json{{"symbol_name", symbol.symbol_name}, {"callers", callers}});
        }

        return TextContent(Json{
            {"target_file", result.target_file},
            {"affected_files", affected_files},
            {"affected_symbols", affected_symbols},
        }.dump());
    }

    if (name == "changed_impact_analysis") {
        if (options.impact_analyzer == nullptr || options.backend_registry == nullptr) {
            return ToolError("changed_impact_analysis is not available (no ImpactAnalyzer/BackendRegistry configured)");
        }

        // "git.diff.head" (working tree vs HEAD), not "git.diff" -- it's
        // the one GitBackend query whose new-side line numbers always
        // match the actual current file content regardless of what's
        // staged vs unstaged, which is what correlating against
        // SymbolIndex needs (see GitBackend.hpp's own comment on Handle()).
        Query query;
        query.backend_id = "core.git";
        query.name = "git.diff.head";
        const auto diff_result = options.backend_registry->RunQuery(query);
        if (!diff_result) {
            return ToolError("changed_impact_analysis: git.diff.head query failed: " + diff_result.Err().message);
        }
        const auto diff_text = std::any_cast<std::string>(diff_result.Value());

        const auto result = options.impact_analyzer->AnalyzeChanges(diff_text);

        Json changed_files = Json::array();
        for (const auto& f : result.changed_files) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(f)) {
                continue;
            }
            changed_files.push_back(f);
        }

        Json affected_files = Json::array();
        for (const auto& f : result.affected_files) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(f)) {
                continue;
            }
            affected_files.push_back(f);
        }

        Json changed_symbols = Json::array();
        for (const auto& symbol : result.changed_symbols) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(symbol.file_path)) {
                continue;
            }
            Json callers = Json::array();
            for (const auto& caller : symbol.callers) {
                if (options.firewall != nullptr && !options.firewall->IsAllowed(caller.caller_file)) {
                    continue;
                }
                callers.push_back(Json{
                    {"caller_name", caller.caller_name},
                    {"caller_file", caller.caller_file},
                    {"line", caller.line},
                    {"callee_text", caller.callee_text},
                });
            }
            changed_symbols.push_back(Json{
                {"symbol_name", symbol.symbol_name},
                {"kind", ToString(symbol.kind)},
                {"file_path", symbol.file_path},
                {"callers", callers},
            });
        }

        return TextContent(Json{
            {"changed_files", changed_files},
            {"changed_symbols", changed_symbols},
            {"affected_files", affected_files},
        }.dump());
    }

    if (name == "project_rules") {
        if (options.backend_registry == nullptr) {
            return ToolError("project_rules is not available (no BackendRegistry configured)");
        }

        Query query;
        query.backend_id = "core.project_rules";
        query.name = "rules.get";
        const auto result = options.backend_registry->RunQuery(query);
        if (!result) {
            return ToolError("project_rules: rules.get query failed: " + result.Err().message);
        }
        return TextContent(std::any_cast<std::string>(result.Value()));
    }

    if (name == "similar_change_search") {
        if (options.backend_registry == nullptr) {
            return ToolError("similar_change_search is not available (no BackendRegistry configured)");
        }
        if (!arguments.contains("symbol_name") || !arguments["symbol_name"].is_string() ||
            arguments["symbol_name"].get<std::string>().empty()) {
            return ToolError("similar_change_search requires a non-empty string 'symbol_name' argument");
        }
        const auto symbol_name = arguments["symbol_name"].get<std::string>();

        Query log_query;
        log_query.backend_id = "core.git";
        log_query.name = "git.log.symbol";
        log_query.parameters = std::any(symbol_name);
        const auto log_result = options.backend_registry->RunQuery(log_query);
        if (!log_result) {
            return ToolError("similar_change_search: git.log.symbol query failed: " + log_result.Err().message);
        }
        const auto commits = std::any_cast<std::vector<GitCommitEntry>>(log_result.Value());

        constexpr std::size_t kMaxCommits = 5;
        constexpr std::size_t kMaxDiffChars = 4000;
        Json commit_list = Json::array();
        for (std::size_t i = 0; i < commits.size() && i < kMaxCommits; ++i) {
            const auto& commit = commits[i];

            std::string diff_text;
            Query show_query;
            show_query.backend_id = "core.git";
            show_query.name = "git.show";
            show_query.parameters = std::any(commit.sha);
            if (const auto show_result = options.backend_registry->RunQuery(show_query); show_result) {
                diff_text = std::any_cast<std::string>(show_result.Value());
            }
            if (diff_text.size() > kMaxDiffChars) {
                // Utf8SafeTruncationLength, not a raw resize(kMaxDiffChars)
                // -- see Core/Git/GitBackend.cpp's identical fix
                // (docs/ROADMAP.md CE-5) for why a naive byte-count cut
                // can produce invalid UTF-8 here.
                diff_text.resize(Utf8SafeTruncationLength(diff_text, kMaxDiffChars));
                diff_text += "\n... [truncated]";
            }

            commit_list.push_back(Json{
                {"sha", commit.sha},
                {"date", commit.date},
                {"subject", commit.subject},
                {"diff", diff_text},
            });
        }

        return TextContent(Json{{"symbol_name", symbol_name}, {"commits", commit_list}}.dump());
    }

    if (name == "branch_impact_analysis") {
        if (options.impact_analyzer == nullptr || options.backend_registry == nullptr) {
            return ToolError("branch_impact_analysis is not available (no ImpactAnalyzer/BackendRegistry configured)");
        }
        if (!arguments.contains("base_branch") || !arguments["base_branch"].is_string() ||
            arguments["base_branch"].get<std::string>().empty()) {
            return ToolError("branch_impact_analysis requires a non-empty string 'base_branch' argument");
        }
        const auto base_branch = arguments["base_branch"].get<std::string>();

        Query query;
        query.backend_id = "core.git";
        query.name = "git.diff.branch";
        query.parameters = std::any(base_branch);
        const auto diff_result = options.backend_registry->RunQuery(query);
        if (!diff_result) {
            return ToolError("branch_impact_analysis: git.diff.branch query failed: " + diff_result.Err().message);
        }
        const auto diff_text = std::any_cast<std::string>(diff_result.Value());

        const auto result = options.impact_analyzer->AnalyzeChanges(diff_text);

        Json changed_files = Json::array();
        for (const auto& f : result.changed_files) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(f)) {
                continue;
            }
            changed_files.push_back(f);
        }

        Json affected_files = Json::array();
        for (const auto& f : result.affected_files) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(f)) {
                continue;
            }
            affected_files.push_back(f);
        }

        Json changed_symbols = Json::array();
        for (const auto& symbol : result.changed_symbols) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(symbol.file_path)) {
                continue;
            }
            Json callers = Json::array();
            for (const auto& caller : symbol.callers) {
                if (options.firewall != nullptr && !options.firewall->IsAllowed(caller.caller_file)) {
                    continue;
                }
                callers.push_back(Json{
                    {"caller_name", caller.caller_name},
                    {"caller_file", caller.caller_file},
                    {"line", caller.line},
                    {"callee_text", caller.callee_text},
                });
            }
            changed_symbols.push_back(Json{
                {"symbol_name", symbol.symbol_name},
                {"kind", ToString(symbol.kind)},
                {"file_path", symbol.file_path},
                {"callers", callers},
            });
        }

        return TextContent(Json{
            {"base_branch", base_branch},
            {"changed_files", changed_files},
            {"changed_symbols", changed_symbols},
            {"affected_files", affected_files},
        }.dump());
    }

    if (name == "git_commit" || name == "git_branch" || name == "git_stash" || name == "git_checkout" ||
        name == "git_tag") {
        if (options.backend_registry == nullptr || !options.enable_git_write_commands) {
            return ToolError(name + " is not available (git write commands are disabled -- see "
                                     "mcp.enable_git_write_commands)");
        }

        Command command;
        command.backend_id = "core.git";
        if (name == "git_commit") {
            if (!arguments.contains("message") || !arguments["message"].is_string() ||
                arguments["message"].get<std::string>().empty()) {
                return ToolError("git_commit requires a non-empty string 'message' argument");
            }
            command.name = "git.commit";
            command.payload = std::any(arguments["message"].get<std::string>());
        } else if (name == "git_branch") {
            if (!arguments.contains("name") || !arguments["name"].is_string() ||
                arguments["name"].get<std::string>().empty()) {
                return ToolError("git_branch requires a non-empty string 'name' argument");
            }
            command.name = "git.branch";
            command.payload = std::any(arguments["name"].get<std::string>());
        } else if (name == "git_checkout") {
            if (!arguments.contains("branch") || !arguments["branch"].is_string() ||
                arguments["branch"].get<std::string>().empty()) {
                return ToolError("git_checkout requires a non-empty string 'branch' argument");
            }
            command.name = "git.checkout";
            command.payload = std::any(arguments["branch"].get<std::string>());
        } else if (name == "git_tag") {
            if (!arguments.contains("name") || !arguments["name"].is_string() ||
                arguments["name"].get<std::string>().empty()) {
                return ToolError("git_tag requires a non-empty string 'name' argument");
            }
            command.name = "git.tag";
            command.payload = std::any(arguments["name"].get<std::string>());
        } else {
            command.name = "git.stash";
            if (arguments.contains("message") && arguments["message"].is_string()) {
                command.payload = std::any(arguments["message"].get<std::string>());
            }
        }

        const auto result = options.backend_registry->Dispatch(command);
        if (!result) {
            return ToolError(name + " failed: " + result.Err().message);
        }
        return TextContent(std::any_cast<std::string>(result.Value()));
    }

    if (name == "active_document") {
        if (options.editor_state_store == nullptr) {
            return ToolError("active_document is not available (no EditorStateStore configured)");
        }

        const auto state = options.editor_state_store->Read();
        if (!state.has_value()) {
            // No IDE extension currently reporting -- a normal condition
            // (matches textDocument/definition's own "well-formed null
            // result, not an error" convention for LspServer.cpp), not a
            // tool failure.
            return TextContent(Json{{"active_document_path", nullptr}, {"selection", nullptr}}.dump());
        }

        Json result = Json{
            {"source", state->source},
            {"active_document_path",
             state->active_document_path.has_value() ? Json(*state->active_document_path) : Json(nullptr)},
            {"updated_at", state->updated_at},
        };
        if (state->selection.has_value()) {
            result["selection"] = Json{
                {"start_line", state->selection->start_line},
                {"start_character", state->selection->start_character},
                {"end_line", state->selection->end_line},
                {"end_character", state->selection->end_character},
            };
        } else {
            result["selection"] = nullptr;
        }
        return TextContent(result.dump());
    }

    return ToolError("unknown tool: " + name);
}

} // namespace

void McpServer::Run(std::istream& in, std::ostream& out) const {
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        Json request;
        try {
            request = Json::parse(line);
        } catch (const std::exception&) {
            out << MakeError(nullptr, -32700, "Parse error").dump() << "\n";
            out.flush();
            continue;
        }

        if (!request.is_object()) {
            out << MakeError(nullptr, -32600, "Invalid Request").dump() << "\n";
            out.flush();
            continue;
        }

        const bool has_id = request.contains("id");
        const Json id = has_id ? request["id"] : Json(nullptr);

        try {
            const auto method = request.value("method", std::string());

            // Per JSON-RPC 2.0, a Request object without an "id" member is
            // a Notification and MUST NOT receive a response — this
            // includes MCP's own "notifications/*" methods (e.g.
            // "notifications/initialized") as well as any other
            // id-less message a client happens to send.
            if (!has_id) {
                continue;
            }

            if (method == "initialize") {
                out << MakeResult(id, Json{
                                           {"protocolVersion", "2024-11-05"},
                                           {"capabilities", Json{{"tools", Json::object()}}},
                                           {"serverInfo", Json{{"name", options_.server_name},
                                                                {"version", options_.server_version}}},
                                       })
                           .dump()
                    << "\n";
            } else if (method == "ping") {
                out << MakeResult(id, Json::object()).dump() << "\n";
            } else if (method == "tools/list") {
                out << MakeResult(id, Json{{"tools", ToolsList(options_)}}).dump() << "\n";
            } else if (method == "tools/call") {
                const auto params = request.value("params", Json::object());
                const auto tool_name = params.value("name", std::string());
                const auto arguments = params.contains("arguments") && params["arguments"].is_object()
                                            ? params["arguments"]
                                            : Json::object();
                out << MakeResult(id, CallTool(options_, tool_name, arguments)).dump() << "\n";
            } else {
                out << MakeError(id, -32601, "Method not found: " + method).dump() << "\n";
            }
            out.flush();
        } catch (const std::exception& e) {
            out << MakeError(id, -32600, std::string("Invalid Request: ") + e.what()).dump() << "\n";
            out.flush();
        }
    }
}

} // namespace aistudio::core
