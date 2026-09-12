#include "Core/LSP/LspServer.hpp"

#include "Core/Index/IncludeEdge.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Search/SymbolSearch.hpp"
#include "Core/Util/Identifier.hpp"
#include "Core/Util/Utf16.hpp"
#include "Core/Util/Utf8.hpp"

// Vendored header-only third-party library (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since this header isn't ours to fix --
// same pattern as Core/src/MCP/McpServer.cpp.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// Vendored third-party parsing library (docs/DEPENDENCY_MANAGEMENT.md),
// same as Core/Index/{Ast,Symbol,Include}Extractor.cpp -- used here only
// by DetectParseError (see its own comment) for the parse-failure half of
// textDocument/publishDiagnostics.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <tree_sitter/api.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

// tree-sitter-cpp's own public entry point (see SymbolExtractor.cpp for
// why this isn't vendored separately).
extern "C" const TSLanguage* tree_sitter_cpp(void);

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace aistudio::core {

namespace {

using Json = nlohmann::json;
namespace fs = std::filesystem;

Json MakeResult(const Json& id, Json result) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

Json MakeError(const Json& id, int code, const std::string& message) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"error", Json{{"code", code}, {"message", message}}}};
}

// A server-initiated NOTIFICATION -- deliberately has no "id" field at
// all (not even `null`), matching JSON-RPC 2.0's own definition of a
// notification and distinguishing it from MakeResult/MakeError's request
// RESPONSES above. Framed through the exact same WriteMessage() call
// site every response already uses (see PublishDiagnosticsIfChanged in
// LspServer.cpp) -- textDocument/publishDiagnostics is this server's
// first use of this shape (see LspServer.hpp's own comment on it).
Json MakeNotification(const std::string& method, Json params) {
    return Json{{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

// ---------------------------------------------------------------------
// Content-Length framing (LSP Base Protocol) -- see LspServer.hpp's own
// doc comment on why this is NOT McpServer's newline-delimited framing.
// ---------------------------------------------------------------------

bool IEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

std::string Trim(const std::string& s) {
    std::size_t start = 0;
    std::size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
        ++start;
    }
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return s.substr(start, end - start);
}

// Upper bound on an accepted Content-Length value, in bytes. Every
// message this server sends or receives is JSON text (a definition
// response, a document's full text on didOpen/didChange, a
// workspace/symbol result list, ...); 64 MiB is generously above any of
// those in real use, so anything past it is treated as malformed framing
// rather than a legitimate huge message. This bound is also what makes a
// negative Content-Length safe to reject: std::stoul() parses a leading
// '-' via strtoul()'s own documented wraparound semantics (i.e. it does
// NOT throw for "-1" -- it silently produces a huge unsigned value, here
// ULONG_MAX), so a value that would otherwise slip through as a
// near-ULONG_MAX content_length is caught by this same bound check before
// the `std::string body(content_length, '\0')` allocation below ever
// runs, instead of surfacing as an uncaught std::bad_alloc/length_error
// that would crash the whole --lsp process on one malformed frame.
constexpr std::size_t kMaxContentLength = 64 * 1024 * 1024; // 64 MiB

// Reads one Content-Length-framed message's body from `in`. Returns
// false on EOF or malformed framing (no Content-Length header seen
// before the blank line, a Content-Length that's negative/non-numeric/
// exceeds kMaxContentLength, or the stream ends before all Content-Length
// bytes are available) -- all of these end LspServer::Run's loop, the
// same way McpServer::Run's `while (std::getline(...))` ends on EOF.
bool ReadMessage(std::istream& in, std::string& out_body) {
    std::size_t content_length = 0;
    bool have_length = false;
    std::string header_line;
    while (std::getline(in, header_line)) {
        if (!header_line.empty() && header_line.back() == '\r') {
            header_line.pop_back();
        }
        if (header_line.empty()) {
            break; // blank line ends the header block
        }
        const auto colon = header_line.find(':');
        if (colon == std::string::npos) {
            continue; // malformed header line -- ignore rather than abort the whole message
        }
        const std::string name = Trim(header_line.substr(0, colon));
        const std::string value = Trim(header_line.substr(colon + 1));
        if (IEquals(name, "Content-Length")) {
            // Reject a leading '-' outright rather than handing it to
            // std::stoul: per strtoul() semantics stoul("-1") does NOT
            // throw, it wraps around to a huge unsigned value -- see
            // kMaxContentLength's own comment above.
            if (value.empty() || value[0] == '-') {
                return false;
            }
            try {
                const auto parsed = std::stoul(value);
                if (parsed > kMaxContentLength) {
                    return false; // absurdly large -- reject before allocating a body this size
                }
                content_length = static_cast<std::size_t>(parsed);
                have_length = true;
            } catch (const std::exception&) {
                return false;
            }
        }
        // Content-Type (if present) is ignored -- every message this
        // server sends or expects is utf-8 JSON, the only encoding real
        // LSP clients use in practice.
    }
    if (!have_length) {
        return false; // EOF before any headers, or no Content-Length seen
    }
    std::string body(content_length, '\0');
    if (content_length > 0) {
        in.read(body.data(), static_cast<std::streamsize>(content_length));
        if (static_cast<std::size_t>(in.gcount()) != content_length) {
            return false; // truncated -- stream ended mid-body
        }
    }
    out_body = std::move(body);
    return true;
}

void WriteMessage(std::ostream& out, const Json& message) {
    const std::string body = message.dump();
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    out.flush();
}

// ---------------------------------------------------------------------
// project-relative path -> `file://` URI conversion, for turning a
// resolved Symbol::file_path into the Location this server sends back to
// the client. There is deliberately no URI -> path direction here: the
// incoming document's own URI is never decoded to a filesystem path --
// `documents` is keyed by the raw URI string as received (see
// HandleDidOpen/HandleDidChange/HandleDidClose/HandleDefinition below),
// since textDocument/definition resolves purely by symbol NAME
// (SymbolIndex::FindByName) across the whole project, never by
// correlating back to which file the cursor was in. Deliberately handles
// only the ASCII/percent-encoding subset real editors send for local
// filesystem paths, not the full RFC 3986 URI grammar.
// ---------------------------------------------------------------------

std::string PercentEncodePath(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (const unsigned char c : s) {
        const bool unreserved = (std::isalnum(c) != 0) || c == '-' || c == '_' || c == '.' || c == '~';
        // '/' and ':' (the latter for a Windows drive letter, e.g.
        // "C:") are structural path separators, not data -- left
        // unescaped like every real `file://` URI does.
        if (unreserved || c == '/' || c == ':') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 0x0F];
        }
    }
    return out;
}

std::string ProjectRelativePathToUri(const std::string& relative_path, const std::string& project_root) {
    std::error_code ec;
    const auto joined = Utf8ToPath(project_root) / Utf8ToPath(relative_path);
    const auto canonical = fs::weakly_canonical(joined, ec);
    const std::string generic = PathToUtf8Generic(!ec ? canonical : joined);
    std::string uri = "file://";
#if defined(_WIN32)
    if (!generic.empty() && generic[0] != '/') {
        uri += '/'; // "file:///C:/..." -- add the URI form's own leading slash before a drive letter
    }
#endif
    uri += PercentEncodePath(generic);
    return uri;
}

// Inverse of PercentEncodePath -- decodes "%XX" escapes back to their raw
// byte. Malformed escapes (a trailing '%' with no two following hex
// digits) are left as literal text rather than rejected, the same
// leniency ReadMessage/GetLine already apply elsewhere in this file to
// input this server doesn't fully control.
std::string PercentDecodePath(const std::string& s) {
    auto hex_digit = [](char c) -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int high = hex_digit(s[i + 1]);
            const int low = hex_digit(s[i + 2]);
            if (high >= 0 && low >= 0) {
                out += static_cast<char>((high << 4) | low);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// document `file://` URI -> project-relative path, the reverse of
// ProjectRelativePathToUri above -- needed only by
// textDocument/documentSymbol, to know which SymbolIndex entries
// (indexed by project-relative Symbol::file_path) belong to the
// requested document. Deliberately handles only the same
// ASCII/percent-encoding local-filesystem-path subset
// ProjectRelativePathToUri produces, not the full RFC 3986 grammar (see
// that function's own comment). Returns nullopt for anything that isn't
// a resolvable local file:// URI under `project_root` -- a non-file URI,
// or a path outside the project root, has nothing in SymbolIndex to find
// either way.
std::optional<std::string> UriToProjectRelativePath(const std::string& uri, const std::string& project_root) {
    static constexpr std::string_view kFilePrefix = "file://";
    if (uri.rfind(kFilePrefix, 0) != 0) {
        return std::nullopt;
    }
    std::string path_part = PercentDecodePath(uri.substr(kFilePrefix.size()));
#if defined(_WIN32)
    if (path_part.size() >= 3 && path_part[0] == '/' && std::isalpha(static_cast<unsigned char>(path_part[1])) != 0 &&
        path_part[2] == ':') {
        path_part.erase(path_part.begin()); // "/C:/..." -> "C:/..." -- undo the leading slash added for the drive letter
    }
#endif

    std::error_code target_ec;
    std::error_code root_ec;
    const auto target = fs::weakly_canonical(Utf8ToPath(path_part), target_ec);
    const auto root = fs::weakly_canonical(Utf8ToPath(project_root), root_ec);
    if (target_ec || root_ec) {
        return std::nullopt;
    }

    const auto relative = target.lexically_relative(root);
    const std::string relative_str = PathToUtf8Generic(relative);
    if (relative.empty() || relative_str.rfind("..", 0) == 0) {
        return std::nullopt; // outside project_root -- SymbolIndex has nothing under it
    }
    return relative_str;
}

// ---------------------------------------------------------------------
// Document text helpers.
// ---------------------------------------------------------------------

// Zero-based line lookup, tolerant of both "\n" and "\r\n" line endings.
// Returns an empty string (not nullopt) for a line number past the end
// of `text` -- callers already treat "no such line" as "no definition
// found" rather than an error, the same leniency Utf16.hpp's own
// conversions apply to an out-of-range column.
std::string GetLine(const std::string& text, int zero_based_line) {
    if (zero_based_line < 0) {
        return {};
    }
    std::size_t pos = 0;
    int current = 0;
    while (current < zero_based_line) {
        const auto newline = text.find('\n', pos);
        if (newline == std::string::npos) {
            return {};
        }
        pos = newline + 1;
        ++current;
    }
    const auto newline = text.find('\n', pos);
    std::string line = newline == std::string::npos ? text.substr(pos) : text.substr(pos, newline - pos);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

std::optional<std::string> ReadFileToString(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// ---------------------------------------------------------------------
// LSP lifecycle / document sync / textDocument/definition.
// ---------------------------------------------------------------------

using DocumentStore = std::map<std::string, std::string>; // uri -> full text

Json InitializeResult(const LspServerOptions& options) {
    Json capabilities = Json::object();
    // TextDocumentSyncKind.Full = 1 -- whole-document replacement per
    // textDocument/didChange, no incremental Range-based sync (this
    // task's deliberately narrowed scope; see LspServer.hpp).
    capabilities["textDocumentSync"] = Json{{"openClose", true}, {"change", 1}};
    if (options.symbol_index != nullptr && !options.project_root.empty()) {
        capabilities["definitionProvider"] = true;
        capabilities["documentSymbolProvider"] = true;
        capabilities["workspaceSymbolProvider"] = true;
        capabilities["referencesProvider"] = true;
        capabilities["typeDefinitionProvider"] = true;
        // renameProvider is additionally gated by enable_rename (opt-in,
        // default false -- see LspServerOptions::enable_rename's own
        // comment) on top of the symbol_index/project_root gate every
        // other capability above already needs. `prepareProvider: true`
        // advertises textDocument/prepareRename (HandlePrepareRename
        // below) alongside it, since both share the exact same
        // "resolve the identifier at the cursor" first step.
        if (options.enable_rename) {
            capabilities["renameProvider"] = Json{{"prepareProvider", true}};
        }
    }
    return Json{
        {"capabilities", capabilities},
        {"serverInfo", Json{{"name", options.server_name}, {"version", options.server_version}}},
    };
}

void HandleDidOpen(const Json& params, DocumentStore& documents) {
    if (!params.contains("textDocument")) {
        return;
    }
    const auto& text_document = params["textDocument"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return;
    }
    documents[text_document["uri"].get<std::string>()] = text_document.value("text", std::string());
}

void HandleDidChange(const Json& params, DocumentStore& documents) {
    if (!params.contains("textDocument") || !params.contains("contentChanges")) {
        return;
    }
    const auto& text_document = params["textDocument"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return;
    }
    const auto& changes = params["contentChanges"];
    if (!changes.is_array() || changes.empty()) {
        return;
    }
    // Whole-document sync only (textDocumentSync.change = Full): the
    // spec requires every element of `contentChanges` to itself be a
    // full-document replacement (no `range` field) when the server
    // advertised Full sync rather than Incremental, so only the LAST one
    // need be applied here.
    const auto& last_change = changes.back();
    if (last_change.contains("text") && last_change["text"].is_string()) {
        documents[text_document["uri"].get<std::string>()] = last_change["text"].get<std::string>();
    }
}

void HandleDidClose(const Json& params, DocumentStore& documents) {
    if (!params.contains("textDocument")) {
        return;
    }
    const auto& text_document = params["textDocument"];
    if (text_document.contains("uri") && text_document["uri"].is_string()) {
        documents.erase(text_document["uri"].get<std::string>());
    }
}

// Finds the first WHOLE-WORD occurrence of `name` in `line_text` at or
// after `start_pos` -- a match not immediately preceded or followed by
// another identifier character (per IsIdentifierChar,
// Core/Util/Identifier.hpp). Plain std::string::find would also match
// `name` as a mere substring of a longer identifier it's a prefix/suffix
// of -- e.g. searching for "Compute" on a line declaring
// "int ComputeAll(int Compute)" would otherwise match inside
// "ComputeAll" first, well before the real "Compute" parameter later on
// the same line. Returns nullopt if no whole-word match exists at or
// after `start_pos`. `start_pos` (default 0) lets FindAllWholeWords below
// re-call this in a loop to enumerate every occurrence on a line, rather
// than duplicating the same boundary-check logic.
std::optional<std::size_t> FindWholeWord(const std::string& line_text, const std::string& name,
                                          std::size_t start_pos = 0) {
    if (name.empty()) {
        return std::nullopt;
    }
    std::size_t pos = start_pos;
    while (true) {
        const auto found = line_text.find(name, pos);
        if (found == std::string::npos) {
            return std::nullopt;
        }
        const bool left_ok = found == 0 || !IsIdentifierChar(line_text[found - 1]);
        const std::size_t after = found + name.size();
        const bool right_ok = after >= line_text.size() || !IsIdentifierChar(line_text[after]);
        if (left_ok && right_ok) {
            return found;
        }
        pos = found + 1; // keep scanning past this partial match
    }
}

// Every WHOLE-WORD occurrence of `name` in `line_text`, in order -- used
// by HandleReferences below, where (unlike LocationFor, which only ever
// needs one declaration's own occurrence) a single line can legitimately
// contain more than one reference to the same name (e.g.
// "return Compute(Compute(x));").
std::vector<std::size_t> FindAllWholeWords(const std::string& line_text, const std::string& name) {
    std::vector<std::size_t> positions;
    std::size_t pos = 0;
    while (const auto found = FindWholeWord(line_text, name, pos)) {
        positions.push_back(*found);
        pos = *found + name.size();
    }
    return positions;
}

// Builds the LSP Location for one matched Symbol. Reads `symbol.file_path`
// off disk (the same content SymbolIndex itself was built from) rather
// than from `documents` -- SymbolIndex's own `symbol.line` was computed
// against that on-disk content, so re-deriving the column from anything
// else (e.g. a currently-open, possibly-edited buffer for that same
// file) could point at the wrong place entirely. This does mean a
// definition target with unsaved edits not yet re-indexed can point at a
// stale line -- an existing, pre-existing limitation of using a
// point-in-time SymbolIndex with no live re-indexing wired up for LSP
// yet (matching this project's own already-documented FileWatcher-wiring
// gap elsewhere), not something this task's scope needs to solve.
Json LocationFor(const Symbol& symbol, const std::string& project_root) {
    const int line0 = symbol.line > 0 ? symbol.line - 1 : 0;

    std::size_t start_char = 0;
    std::size_t end_char = 0;
    if (const auto content = ReadFileToString(Utf8ToPath(project_root) / Utf8ToPath(symbol.file_path));
        content.has_value()) {
        const std::string line_text = GetLine(*content, line0);
        if (const auto byte_pos = FindWholeWord(line_text, symbol.name); byte_pos.has_value()) {
            start_char = Utf8ByteToUtf16Offset(line_text, *byte_pos);
            end_char = Utf8ByteToUtf16Offset(line_text, *byte_pos + symbol.name.size());
        }
        // If the name text isn't found on that line (e.g. the file
        // changed since SymbolIndex was last built), fall back to a
        // zero-width range at the line's start rather than guessing.
    }

    return Json{
        {"uri", ProjectRelativePathToUri(symbol.file_path, project_root)},
        {"range",
         Json{
             {"start", Json{{"line", line0}, {"character", start_char}}},
             {"end", Json{{"line", line0}, {"character", end_char}}},
         }},
    };
}

// Resolves textDocument/definition. Deliberately the same
// "heuristic, exact-name match, no full semantic/type resolution"
// approach this codebase already uses for CallGraph/ReferenceGraph
// (see their own header comments) rather than new symbol-resolution
// machinery: the identifier text under the cursor is looked up directly
// via SymbolIndex::FindByName(), an EXACT match. Known limitation this
// implies: an out-of-line member function definition's Symbol::name is
// stored qualified (e.g. "Foo::Bar" -- see SymbolExtractor.cpp), but the
// identifier extracted from a call site's cursor position is only the
// unqualified segment ("Bar") for a member-access/qualified expression
// (IdentifierAt() -- like TrailingIdentifier() it deliberately stops at
// non-identifier characters, and "::" is not an identifier character) --
// so a call site resolving to such an out-of-line definition won't
// currently be found this way. Free functions, classes/structs,
// namespaces, and inline member definitions (whose Symbol::name is
// already unqualified) all resolve correctly. Returns JSON `null`
// (Location[] being empty is represented the same way, per the LSP spec
// allowing either) whenever nothing can be resolved.
Json HandleDefinition(const LspServerOptions& options, const DocumentStore& documents, const Json& params) {
    if (options.symbol_index == nullptr || options.project_root.empty()) {
        return Json(nullptr);
    }
    if (!params.contains("textDocument") || !params.contains("position")) {
        return Json(nullptr);
    }
    const auto& text_document = params["textDocument"];
    const auto& position = params["position"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return Json(nullptr);
    }
    if (!position.contains("line") || !position.contains("character") || !position["line"].is_number_integer() ||
        !position["character"].is_number_integer()) {
        return Json(nullptr);
    }

    const auto doc_it = documents.find(text_document["uri"].get<std::string>());
    if (doc_it == documents.end()) {
        return Json(nullptr); // only in-memory-synced (didOpen'd) documents are searchable
    }

    const int line = position["line"].get<int>();
    const auto character_value = position["character"].get<std::int64_t>();
    const auto character = static_cast<std::size_t>(character_value < 0 ? 0 : character_value);

    const std::string line_text = GetLine(doc_it->second, line);
    const std::size_t byte_offset = Utf16OffsetToUtf8Byte(line_text, character);
    const std::string word = IdentifierAt(line_text, byte_offset);
    if (word.empty()) {
        return Json(nullptr);
    }

    Json locations = Json::array();
    for (const auto& symbol : options.symbol_index->FindByName(word)) {
        if (options.firewall != nullptr && !options.firewall->IsAllowed(symbol.file_path)) {
            continue;
        }
        locations.push_back(LocationFor(symbol, options.project_root));
    }
    if (locations.empty()) {
        return Json(nullptr);
    }
    return locations;
}

// Resolves textDocument/typeDefinition.
//
// INVESTIGATION (docs/ROADMAP.md's Current Status entry for this task has
// the full writeup): a prior task's report claimed "Type needs real type
// inference beyond tree-sitter's syntax-only parsing" without actually
// checking. That claim was verified here by reading SymbolExtractor.cpp
// in full: tree-sitter-cpp's own declaration/field_declaration/
// function_definition nodes expose a "type" field directly, alongside the
// "declarator" field FindNameNode/FindVariableNameNode already read
// structurally to get a symbol's name. Symbol::type_name (Symbol.hpp) now
// captures that field's text whenever it's a plain `type_identifier` --
// e.g. "Foo" in both "Foo x;" and "Foo Bar();" -- at the exact same
// extraction time as everything else SymbolIndex already indexes. This
// means typeDefinition below is NOT a text heuristic (e.g. "read the
// token before the cursor and guess it's a type name", which was
// considered and rejected -- see LspServer.hpp's own note and this
// function's "known limitations" below for why guessing was ruled out):
// it is the exact identifier tree-sitter itself parsed as this
// declaration's type, looked up via SymbolIndex::FindByName() as a SECOND
// hop after the SAME first hop textDocument/definition already performs
// (cursor identifier -> SymbolIndex::FindByName()). Two exact-match
// SymbolIndex lookups chained together, no new resolution machinery.
//
// KNOWN LIMITATIONS (a direct consequence of what Symbol::type_name does
// and doesn't capture -- see that field's own doc comment):
//   * A builtin type (`int`/`bool`/`void`/...), a namespace-qualified type
//     (`std::string`), a template instantiation (`std::vector<Foo>`), and
//     `auto` (genuine type deduction, which this project has no semantic
//     analysis for) all leave Symbol::type_name empty -- typeDefinition on
//     such a variable/function returns `null` rather than a guess.
//   * A cursor on a Class/Struct/Namespace symbol itself also resolves to
//     nothing (no "type" of a type) -- this capability is only meaningful
//     starting from a Variable's or Function's own declared type.
//   * Same out-of-line-member qualification gap textDocument/definition
//     already documents: IdentifierAt() extracts only the unqualified
//     segment at the cursor, so resolving the DECLARING symbol at a
//     member-access call site (e.g. `obj.field`, cursor on `field`) shares
//     Definition's existing limitation. This is unaffected by
//     type_name itself, only by how the first hop's identifier is found.
//   * Multiple same-named declaring symbols (SymbolIndex::FindByName's own
//     existing ambiguity, e.g. overloaded free functions) contribute the
//     union of their distinct type_names' resolved locations, the same
//     "return every exact-name match" approach HandleDefinition already
//     takes rather than picking one arbitrarily.
Json HandleTypeDefinition(const LspServerOptions& options, const DocumentStore& documents, const Json& params) {
    if (options.symbol_index == nullptr || options.project_root.empty()) {
        return Json(nullptr);
    }
    if (!params.contains("textDocument") || !params.contains("position")) {
        return Json(nullptr);
    }
    const auto& text_document = params["textDocument"];
    const auto& position = params["position"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return Json(nullptr);
    }
    if (!position.contains("line") || !position.contains("character") || !position["line"].is_number_integer() ||
        !position["character"].is_number_integer()) {
        return Json(nullptr);
    }

    const auto doc_it = documents.find(text_document["uri"].get<std::string>());
    if (doc_it == documents.end()) {
        return Json(nullptr); // only in-memory-synced (didOpen'd) documents are searchable, same as textDocument/definition
    }

    const int line = position["line"].get<int>();
    const auto character_value = position["character"].get<std::int64_t>();
    const auto character = static_cast<std::size_t>(character_value < 0 ? 0 : character_value);

    const std::string line_text = GetLine(doc_it->second, line);
    const std::size_t byte_offset = Utf16OffsetToUtf8Byte(line_text, character);
    const std::string word = IdentifierAt(line_text, byte_offset);
    if (word.empty()) {
        return Json(nullptr);
    }

    Json locations = Json::array();
    std::vector<std::string> resolved_type_names; // avoid duplicate lookups/locations across several FindByName(word) hits sharing a type
    for (const auto& symbol : options.symbol_index->FindByName(word)) {
        if (symbol.type_name.empty()) {
            continue; // builtin/qualified/template/auto -- see this function's own comment
        }
        if (std::find(resolved_type_names.begin(), resolved_type_names.end(), symbol.type_name) !=
            resolved_type_names.end()) {
            continue;
        }
        resolved_type_names.push_back(symbol.type_name);
        for (const auto& type_symbol : options.symbol_index->FindByName(symbol.type_name)) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(type_symbol.file_path)) {
                continue;
            }
            locations.push_back(LocationFor(type_symbol, options.project_root));
        }
    }
    if (locations.empty()) {
        return Json(nullptr);
    }
    return locations;
}

// aistudio::core::SymbolKind -> LSP's `SymbolKind` numeric enum (LSP
// 3.17 Base Protocol, "SymbolKind"). Struct/Namespace/Function/Variable
// map onto LSP's own same-named kinds directly; Class maps to LSP's
// Class (5) rather than Interface/Struct since this project doesn't
// distinguish interfaces from classes (SymbolExtractor.cpp only ever
// produces SymbolKind::Class for a `class` declaration either way).
int LspSymbolKind(SymbolKind kind) {
    switch (kind) {
    case SymbolKind::Class:
        return 5; // Class
    case SymbolKind::Struct:
        return 23; // Struct
    case SymbolKind::Function:
        return 12; // Function
    case SymbolKind::Namespace:
        return 3; // Namespace
    case SymbolKind::Variable:
        return 13; // Variable
    }
    return 13; // unreachable for a valid SymbolKind -- Variable is the least surprising fallback
}

// Builds one LSP `SymbolInformation` (LSP 3.17 -- the flat shape; see
// LspServer.hpp's own comment on why this server never produces the
// hierarchical `DocumentSymbol` shape) for a matched Symbol. Reuses
// LocationFor -- the same disk-read-and-search-the-line approach
// textDocument/definition already uses to turn a line-only Symbol::line
// into a precise column range, and the same existing "SymbolIndex is
// point-in-time, not live-buffer-aware" limitation applies here too (see
// LocationFor's own comment). `containerName` (LSP's optional field for
// "declared inside this class/namespace") is deliberately omitted rather
// than guessed at by splitting on "::" -- Symbol::name's qualification is
// inconsistent across kinds (e.g. an out-of-line member is qualified,
// a namespace-scope function is not), and this task's scope is reusing
// SymbolIndex as-is, not inferring containment data it doesn't track.
Json SymbolInformationFor(const Symbol& symbol, const std::string& project_root) {
    return Json{
        {"name", symbol.name},
        {"kind", LspSymbolKind(symbol.kind)},
        {"location", LocationFor(symbol, project_root)},
    };
}

// Resolves textDocument/documentSymbol: every SymbolIndex entry whose
// Symbol::file_path matches the requested document, as a flat
// SymbolInformation[] (see LspServer.hpp's comment on the DocumentSymbol
// vs SymbolInformation shape decision). Deliberately does NOT consult
// `documents` (the didOpen/didChange-synced in-memory buffers) the way
// HandleDefinition does -- SymbolIndex itself is disk-based (built from
// FileScanner + SymbolExtractor, not from any LSP-synced buffer), so
// there both is nothing buffer-specific to look up and no requirement
// that the document be open at all; only the URI -> project-relative-path
// mapping (UriToProjectRelativePath) is needed to know which entries
// belong to it. Returns JSON `null` (matching HandleDefinition's own
// "empty result set" convention) if the URI can't be resolved under
// project_root, the resolved path is firewalled, or the file simply has
// no indexed symbols.
Json HandleDocumentSymbol(const LspServerOptions& options, const Json& params) {
    if (options.symbol_index == nullptr || options.project_root.empty()) {
        return Json(nullptr);
    }
    if (!params.contains("textDocument")) {
        return Json(nullptr);
    }
    const auto& text_document = params["textDocument"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return Json(nullptr);
    }

    const auto relative_path = UriToProjectRelativePath(text_document["uri"].get<std::string>(), options.project_root);
    if (!relative_path.has_value()) {
        return Json(nullptr);
    }
    if (options.firewall != nullptr && !options.firewall->IsAllowed(*relative_path)) {
        return Json(nullptr);
    }

    Json symbols = Json::array();
    for (const auto& symbol : options.symbol_index->All()) {
        if (symbol.file_path == *relative_path) {
            symbols.push_back(SymbolInformationFor(symbol, options.project_root));
        }
    }
    if (symbols.empty()) {
        return Json(nullptr);
    }
    return symbols;
}

// Resolves workspace/symbol: a project-wide, ranked name search over
// SymbolIndex. Deliberately reuses SymbolSearch (Core/Search/SymbolSearch.hpp)
// as-is rather than adding new query logic to SymbolIndex itself --
// SymbolSearch already implements exactly what workspace/symbol's `query`
// needs (prefix/substring matching with tiered relevance ranking on top
// of SymbolIndex::All(), the same search McpServer's `symbol_search` tool
// already exposes to MCP clients), so this is pure reuse, not a new
// indexing capability. An empty/missing `query` yields no results
// (SymbolSearch::Search's own early return) rather than "list everything"
// -- a workspace-wide dump isn't a useful default and isn't what real LSP
// clients send an empty query for in practice. Returns JSON `null`
// (matching HandleDefinition/HandleDocumentSymbol's own convention) when
// nothing matches.
Json HandleWorkspaceSymbol(const LspServerOptions& options, const Json& params) {
    if (options.symbol_index == nullptr || options.project_root.empty()) {
        return Json(nullptr);
    }
    const auto query = params.value("query", std::string());

    const SymbolSearch search;
    Json symbols = Json::array();
    for (const auto& match : search.Search(*options.symbol_index, query)) {
        if (options.firewall != nullptr && !options.firewall->IsAllowed(match.symbol.file_path)) {
            continue;
        }
        symbols.push_back(SymbolInformationFor(match.symbol, options.project_root));
    }
    if (symbols.empty()) {
        return Json(nullptr);
    }
    return symbols;
}

// Same source-extension allowlist SymbolIndex/ReferenceGraph/CallGraph/
// IncludeGraph/InheritanceGraph/AstIndex each already define as their own
// private IsSourceFile() (see e.g. Core/src/Index/ReferenceGraph.cpp) --
// duplicated here rather than shared, following that same existing
// per-class-local convention, since HandleReferences below scans project
// files directly rather than going through any of those Index classes.
constexpr std::array<const char*, 6> kReferenceSourceExtensions = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx"};

bool IsReferenceSourceFile(const std::string& path) {
    const auto extension = Utf8ToPath(path).extension();
    return std::any_of(kReferenceSourceExtensions.begin(), kReferenceSourceExtensions.end(),
                        [&](const char* candidate) { return extension == candidate; });
}

// Resolves textDocument/references.
//
// INVESTIGATION (docs/ROADMAP.md's Current Status entry for this task has
// the full writeup): this project's two existing "who references this"
// indexes were read in full before deciding how to implement this.
// ReferenceGraph (Core/Index/ReferenceExtractor.cpp) only records
// tree-sitter `type_identifier` nodes -- a variable's declared type, a
// parameter/return type, a base class, a cast target -- and explicitly
// excludes a class/struct's own name at its definition site; it never
// looks at a `namespace_identifier`, and a plain variable read/write is
// neither node kind. CallGraph (Core/Index/CallExtractor.cpp) only
// records a `call_expression`'s callee text, and only when that call is
// textually inside some function body (a call at namespace/global scope,
// e.g. in a static initializer, is silently dropped -- WalkNode's
// `current_caller` starts empty and a call site with an empty
// current_caller is never recorded). Combined, these two indexes give
// real coverage for exactly two of SymbolIndex's five SymbolKinds
// (Class/Struct via ReferenceGraph, Function via CallGraph) and ZERO
// coverage for Variable and Namespace. A References implementation built
// only on them would silently return nothing for two of five symbol
// kinds SymbolIndex itself tracks -- worse and more confusing than a
// uniformly-approximate result, so this handler does NOT use
// ReferenceGraph/CallGraph at all.
//
// IMPLEMENTATION: following this project's own established precedent for
// exactly this class of problem (Core/Search/KeywordSearch.hpp's own doc
// comment calls itself "the most basic of the search modes", a fresh
// per-query FileScanner scan + read with no persistent index), this is a
// plain whole-word, CASE-SENSITIVE (identifiers are case-sensitive in
// C++, unlike KeywordSearch's own deliberate case-insensitive prose
// search) text scan for the cursor's identifier across every source file
// (same kSourceExtensions convention as SymbolIndex/ReferenceGraph/
// CallGraph -- see IsReferenceSourceFile above) FileScanner finds under
// project_root, reusing FindAllWholeWords (itself built on the same
// FindWholeWord LocationFor above already uses) rather than
// reimplementing word-boundary matching.
//
// This is a NAME-TEXT heuristic, NOT scope-aware semantic resolution --
// explicitly documented limitations:
//   * False positives: an unrelated symbol in a different scope/class/
//     namespace that merely happens to share the same spelling is
//     indistinguishable from a real reference from name text alone (the
//     same tradeoff CallGraph::Callers's own header comment already
//     admits to for call sites).
//   * False negatives: a reference produced only through macro expansion
//     will never match this identifier's literal spelling; conversely a
//     mention inside a disabled `#if 0` block (which this plain text scan
//     has no preprocessor awareness of) is indistinguishable from a real
//     one and WILL be included.
//   * Comments and string literals are not distinguished from real code
//     -- confirmed by this task's own real-machine verification (running
//     against this very repository's own source): a plain-English doc
//     comment mentioning "FindWholeWord" by name was returned as a
//     "reference" alongside its actual call sites, since this scan (like
//     tree-sitter-free KeywordSearch) never parses the file at all.
// This trades ReferenceGraph/CallGraph's tree-sitter-verified precision
// for uniform coverage across every SymbolKind -- per the investigation
// above, the coverage gap was judged the worse defect for a
// general-purpose References capability (this project already treats
// "heuristic name-text matching, not semantic resolution" as an accepted
// tradeoff elsewhere -- see ReferenceGraph/CallGraph's own header
// comments).
//
// `context.includeDeclaration` (LSP spec, defaults to true here if the
// client omits it) controls whether the symbol's own declaration site(s)
// -- from SymbolIndex::FindByName(word), the same exact-name lookup
// textDocument/definition already uses -- are included among the
// results. A text-scan hit is identified as "the declaration" by exact
// (file_path, zero-based line, byte offset) match against a declaration
// site computed via the same GetLine+FindWholeWord path LocationFor
// already uses, so the two always agree on where a declaration's own
// occurrence sits.
Json HandleReferences(const LspServerOptions& options, const DocumentStore& documents, const Json& params) {
    if (options.symbol_index == nullptr || options.project_root.empty()) {
        return Json(nullptr);
    }
    if (!params.contains("textDocument") || !params.contains("position")) {
        return Json(nullptr);
    }
    const auto& text_document = params["textDocument"];
    const auto& position = params["position"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return Json(nullptr);
    }
    if (!position.contains("line") || !position.contains("character") || !position["line"].is_number_integer() ||
        !position["character"].is_number_integer()) {
        return Json(nullptr);
    }

    const auto doc_it = documents.find(text_document["uri"].get<std::string>());
    if (doc_it == documents.end()) {
        return Json(nullptr); // only in-memory-synced (didOpen'd) documents are searchable, same as textDocument/definition
    }

    const int line = position["line"].get<int>();
    const auto character_value = position["character"].get<std::int64_t>();
    const auto character = static_cast<std::size_t>(character_value < 0 ? 0 : character_value);

    const std::string cursor_line_text = GetLine(doc_it->second, line);
    const std::size_t byte_offset = Utf16OffsetToUtf8Byte(cursor_line_text, character);
    const std::string word = IdentifierAt(cursor_line_text, byte_offset);
    if (word.empty()) {
        return Json(nullptr);
    }

    const bool include_declaration = params.value("context", Json::object()).value("includeDeclaration", true);

    // Declaration sites to exclude below when include_declaration is
    // false -- see this function's own comment on how these are matched
    // against text-scan hits.
    std::vector<std::tuple<std::string, int, std::size_t>> declaration_sites;
    if (!include_declaration) {
        for (const auto& symbol : options.symbol_index->FindByName(word)) {
            if (options.firewall != nullptr && !options.firewall->IsAllowed(symbol.file_path)) {
                continue; // never even read a firewalled declaration site's content
            }
            const int decl_line0 = symbol.line > 0 ? symbol.line - 1 : 0;
            if (const auto content = ReadFileToString(Utf8ToPath(options.project_root) / Utf8ToPath(symbol.file_path));
                content.has_value()) {
                const std::string decl_line_text = GetLine(*content, decl_line0);
                if (const auto byte_pos = FindWholeWord(decl_line_text, symbol.name); byte_pos.has_value()) {
                    declaration_sites.emplace_back(symbol.file_path, decl_line0, *byte_pos);
                }
            }
        }
    }

    const FileScanner scanner;
    const auto scan_result = scanner.Scan(options.project_root);
    if (!scan_result) {
        return Json(nullptr);
    }

    Json locations = Json::array();
    const fs::path root_path = Utf8ToPath(options.project_root);
    for (const auto& metadata : scan_result.Value()) {
        if (!IsReferenceSourceFile(metadata.path)) {
            continue;
        }
        if (options.firewall != nullptr && !options.firewall->IsAllowed(metadata.path)) {
            continue; // never even read a firewalled file's content
        }
        const auto content = ReadFileToString(root_path / Utf8ToPath(metadata.path));
        if (!content.has_value()) {
            continue;
        }

        std::istringstream stream(*content);
        std::string raw_line;
        int line_index = 0; // 0-based, matches LocationFor's own line0 convention
        while (std::getline(stream, raw_line)) {
            if (!raw_line.empty() && raw_line.back() == '\r') {
                raw_line.pop_back();
            }
            for (const std::size_t byte_pos : FindAllWholeWords(raw_line, word)) {
                const bool is_declaration =
                    std::find(declaration_sites.begin(), declaration_sites.end(),
                              std::make_tuple(metadata.path, line_index, byte_pos)) != declaration_sites.end();
                if (is_declaration) {
                    continue; // include_declaration is false here -- declaration_sites is only populated in that case
                }
                const std::size_t start_char = Utf8ByteToUtf16Offset(raw_line, byte_pos);
                const std::size_t end_char = Utf8ByteToUtf16Offset(raw_line, byte_pos + word.size());
                locations.push_back(Json{
                    {"uri", ProjectRelativePathToUri(metadata.path, options.project_root)},
                    {"range",
                     Json{
                         {"start", Json{{"line", line_index}, {"character", start_char}}},
                         {"end", Json{{"line", line_index}, {"character", end_char}}},
                     }},
                });
            }
            ++line_index;
        }
    }

    if (locations.empty()) {
        return Json(nullptr);
    }
    return locations;
}

// ---------------------------------------------------------------------
// textDocument/rename / textDocument/prepareRename (see
// LspServerOptions::enable_rename's own comment in LspServer.hpp for the
// full design writeup: single-FILE scope only, opt-in toggle mirroring
// McpServerOptions::enable_git_write_commands, and this server's own
// conclusion that computing a WorkspaceEdit -- never writing to disk --
// puts this in a materially smaller risk class than a GitBackend write
// Command).
// ---------------------------------------------------------------------

// Whether `name` is syntactically a legal C++ identifier: non-empty,
// every character (IsIdentifierChar, Core/Util/Identifier.hpp) valid,
// and the FIRST character specifically not a digit -- IsIdentifierChar
// alone allows a leading digit ('3' is alnum), which no C++ identifier
// may start with. Used only to reject an obviously-malformed `newName`
// up front (AGENT.md #10 "don't swallow errors" -- a garbage newName
// gets a clear -32602 error, see HandleRename below, rather than
// silently producing a WorkspaceEdit whose newText a client would apply
// as invalid C++, or being accepted and only failing much later when the
// user tries to compile). Does NOT reject a legal identifier that
// happens to be a C++ keyword (e.g. renaming something to "class") --
// this project has no keyword table to check against, and a client-side
// compile/build would surface that mistake anyway; documented as a known
// gap rather than guessed at.
bool IsValidIdentifierName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    if (std::isdigit(static_cast<unsigned char>(name[0])) != 0) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) { return IsIdentifierChar(c); });
}

// One resolved rename target: the identifier under the cursor, already
// validated against every precondition HandleRename and HandlePrepareRename
// share (see this struct's only producer, ResolveRenameTarget, below).
struct RenameTarget {
    std::string uri;           // exactly as the client sent it -- echoed back as WorkspaceEdit.changes' one key
    std::string relative_path; // project-relative path, already firewall-checked
    std::string word;          // the identifier text under the cursor
    int line = 0;               // 0-based, matching LSP's own Position.line
    std::size_t start_char = 0; // UTF-16 code units, matching LSP's own Position.character
    std::size_t end_char = 0;
};

// Shared first step for both textDocument/rename and
// textDocument/prepareRename: resolves `params`'s textDocument+position to
// the identifier under the cursor, applying every precondition either
// capability needs before doing anything rename-SPECIFIC (HandleRename
// alone additionally validates `newName` and builds the actual edits;
// prepareRename never even looks at `newName`, the field isn't part of
// its request shape at all per the LSP spec).
//
// Deliberately returns nullopt (not an error) for every one of these
// preconditions failing -- unlike the enable_rename/newName-validation
// checks in HandleRename, which DO produce explicit JSON-RPC errors (see
// that function's own comment on why those two are treated differently).
// This mirrors HandleDefinition/HandleReferences/HandleTypeDefinition's
// own established convention: "no document open", "no identifier under
// the cursor", "cursor URI outside the project root", and "file denied by
// the Context Firewall" are all ordinary, expected "nothing to resolve"
// outcomes a well-behaved client can hit just by moving its cursor
// around, not malformed requests worth a protocol-level error for.
//
// PRECISION SAFEGUARD (LspServerOptions::enable_rename's own comment has
// the full writeup): requires SymbolIndex::FindByName(word) to be
// non-empty -- the identifier under the cursor must be a name SOME
// declared symbol in the project actually has, the same exact-name
// lookup textDocument/definition already performs. This rejects renaming
// plain text that was never a real symbol (a comment word, a string
// fragment, a typo) but does NOT prove the specific occurrence under the
// cursor IS that declaration -- see HandleRename's own comment on the
// false-positive consequence this leaves.
std::optional<RenameTarget> ResolveRenameTarget(const LspServerOptions& options, const DocumentStore& documents,
                                                 const Json& params) {
    if (options.symbol_index == nullptr || options.project_root.empty()) {
        return std::nullopt;
    }
    if (!params.contains("textDocument") || !params.contains("position")) {
        return std::nullopt;
    }
    const auto& text_document = params["textDocument"];
    const auto& position = params["position"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return std::nullopt;
    }
    if (!position.contains("line") || !position.contains("character") || !position["line"].is_number_integer() ||
        !position["character"].is_number_integer()) {
        return std::nullopt;
    }

    const std::string uri = text_document["uri"].get<std::string>();
    const auto doc_it = documents.find(uri);
    if (doc_it == documents.end()) {
        return std::nullopt; // only in-memory-synced (didOpen'd) documents are renameable, same as definition/references
    }

    // Single-file scope's own enforcement point: the ONLY file this
    // request can ever touch is the one named by `uri` itself -- resolved
    // here purely to firewall-check it and to name it back in the
    // WorkspaceEdit response, never to look up occurrences in any OTHER
    // file the way textDocument/references' project-wide scan does.
    const auto relative_path = UriToProjectRelativePath(uri, options.project_root);
    if (!relative_path.has_value()) {
        return std::nullopt; // not a resolvable local file:// URI under project_root
    }
    if (options.firewall != nullptr && !options.firewall->IsAllowed(*relative_path)) {
        return std::nullopt; // Context Firewall -- same convention as HandleDocumentSymbol's own early-return
    }

    const int line = position["line"].get<int>();
    const auto character_value = position["character"].get<std::int64_t>();
    const auto character = static_cast<std::size_t>(character_value < 0 ? 0 : character_value);

    const std::string line_text = GetLine(doc_it->second, line);
    const std::size_t byte_offset = Utf16OffsetToUtf8Byte(line_text, character);
    const auto range = IdentifierRangeAt(line_text, byte_offset);
    if (!range.has_value()) {
        return std::nullopt; // no identifier touches this cursor position
    }
    const std::string word = line_text.substr(range->first, range->second - range->first);

    if (options.symbol_index->FindByName(word).empty()) {
        return std::nullopt; // precision safeguard -- see this function's own comment
    }

    return RenameTarget{
        uri,
        *relative_path,
        word,
        line,
        Utf8ByteToUtf16Offset(line_text, range->first),
        Utf8ByteToUtf16Offset(line_text, range->second),
    };
}

// Resolves textDocument/prepareRename: an OPTIONAL LSP capability (LSP
// 3.16+) letting a client ask "is renaming valid here, and what's the
// current name?" before ever showing a rename UI or asking the user for
// a new name -- this request has no `newName` field at all. Implemented
// here because it falls directly out of ResolveRenameTarget's own
// "resolve the identifier at the cursor" step already needed for
// HandleRename -- no separate resolution logic. Returns
// `{range, placeholder}` (LSP's own shape: `range` is what the client
// should highlight/replace, `placeholder` is the text to pre-fill the
// rename input with) on success, or JSON `null` when nothing is
// renameable here (ResolveRenameTarget returned nullopt) -- matching
// HandleDefinition/HandleReferences's own "null means no result" idiom,
// since (unlike a submitted `newName`) there is no user-supplied input
// here that could be malformed enough to warrant an actual error.
Json HandlePrepareRename(const LspServerOptions& options, const DocumentStore& documents, const Json& id,
                          const Json& params) {
    if (!options.enable_rename) {
        // Gated identically to HandleRename below (same
        // LspServerOptions::enable_rename toggle, same reasoning) -- a
        // client that calls this anyway despite it not being advertised
        // (no `prepareProvider` in `initialize`'s renameProvider result)
        // gets the same explicit rejection, not a misleading `null`
        // ("nothing renameable here") that would hide WHY.
        return MakeError(id, -32601, "textDocument/prepareRename is disabled (see mcp.enable_lsp_rename)");
    }
    const auto target = ResolveRenameTarget(options, documents, params);
    if (!target.has_value()) {
        return MakeResult(id, Json(nullptr));
    }
    return MakeResult(id, Json{
                               {"range",
                                Json{
                                    {"start", Json{{"line", target->line}, {"character", target->start_char}}},
                                    {"end", Json{{"line", target->line}, {"character", target->end_char}}},
                                }},
                               {"placeholder", target->word},
                           });
}

// Resolves textDocument/rename. Deliberately scoped to exactly the ONE
// file named by the request's own `textDocument.uri` -- per the user's
// explicit decision (docs/ROADMAP.md's Rename entry), this NEVER expands
// to other files the way textDocument/references' project-wide FileScanner
// scan does, structurally bounding the blast radius of a bad rename to
// something a single `git checkout`/editor Undo can fully recover. Every
// WHOLE-WORD occurrence of the cursor's identifier within that one
// document's CURRENT buffer text (FindAllWholeWords, the exact same
// helper HandleReferences already uses -- no new word-boundary matching
// logic) becomes one TextEdit; the whole set is returned as a
// WorkspaceEdit naming only that one URI.
//
// THIS SERVER NEVER WRITES TO DISK: the result is the proposed edit set
// for the CLIENT to apply (or not) -- Run()'s `documents` map (the
// didOpen/didChange-synced buffer) is read here, never mutated, and
// nothing in this function (or anywhere else in LspServer.cpp) opens a
// file for writing. See LspServerOptions::enable_rename's own comment for
// why this makes Rename's real risk profile smaller than a GitBackend
// write Command despite living behind the same opt-in toggle convention.
//
// Unlike every read-only capability in this file (which all fold "toggle
// disabled" into the same `null` result as "nothing found"), a disabled
// or malformed rename request gets an actual JSON-RPC error:
//   * -32601 (Method not found) when enable_rename is false or the
//     symbol_index/project_root gate isn't satisfied -- matching this
//     project's own established "capability not advertised => the spec's
//     MethodNotFound convention" (LspServer.hpp's own comment), since
//     initialize() genuinely never advertised renameProvider in that case.
//   * -32602 (Invalid params) for a missing/non-string `newName`, or one
//     that fails IsValidIdentifierName (empty, or containing characters
//     that aren't valid in a C++ identifier) -- AGENT.md #10 "don't
//     swallow errors": a bad newName is explicitly rejected with a
//     message explaining why, not silently coerced or ignored.
// Every OTHER "nothing to rename" outcome (no document open, no
// identifier under the cursor, the identifier isn't a real declared
// symbol anywhere, the file is firewalled) still returns `null`, via
// ResolveRenameTarget -- these are the same kind of "ordinary, expected"
// outcomes HandleDefinition/HandleReferences already treat as `null`, not
// protocol errors.
Json HandleRename(const LspServerOptions& options, const DocumentStore& documents, const Json& id,
                   const Json& params) {
    if (!options.enable_rename || options.symbol_index == nullptr || options.project_root.empty()) {
        return MakeError(id, -32601, "textDocument/rename is disabled (see mcp.enable_lsp_rename)");
    }
    if (!params.contains("newName") || !params["newName"].is_string()) {
        return MakeError(id, -32602, "textDocument/rename requires a string 'newName' parameter");
    }
    const std::string new_name = params["newName"].get<std::string>();
    if (!IsValidIdentifierName(new_name)) {
        return MakeError(id, -32602,
                          "'newName' must be a non-empty valid C++ identifier (letters, digits, underscore; "
                          "must not start with a digit)");
    }

    const auto target = ResolveRenameTarget(options, documents, params);
    if (!target.has_value()) {
        return MakeResult(id, Json(nullptr));
    }

    const auto doc_it = documents.find(target->uri);
    // ResolveRenameTarget already confirmed `target->uri` is open; re-find
    // rather than have it carry the whole buffer text through the struct.

    Json edits = Json::array();
    std::istringstream stream(doc_it->second);
    std::string raw_line;
    int line_index = 0; // 0-based, matches every other Location this file produces
    while (std::getline(stream, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') {
            raw_line.pop_back();
        }
        for (const std::size_t byte_pos : FindAllWholeWords(raw_line, target->word)) {
            const std::size_t start_char = Utf8ByteToUtf16Offset(raw_line, byte_pos);
            const std::size_t end_char = Utf8ByteToUtf16Offset(raw_line, byte_pos + target->word.size());
            edits.push_back(Json{
                {"range",
                 Json{
                     {"start", Json{{"line", line_index}, {"character", start_char}}},
                     {"end", Json{{"line", line_index}, {"character", end_char}}},
                 }},
                {"newText", new_name},
            });
        }
        ++line_index;
    }

    if (edits.empty()) {
        // Shouldn't happen -- the cursor's own occurrence guarantees at
        // least one whole-word hit on its own line -- but fall back to
        // "nothing to rename" rather than returning a WorkspaceEdit with
        // an empty edit list for this URI.
        return MakeResult(id, Json(nullptr));
    }

    return MakeResult(id, Json{{"changes", Json{{target->uri, edits}}}});
}

// ---------------------------------------------------------------------
// textDocument/publishDiagnostics (see LspServer.hpp's own comment on
// the overall design: v1 scoped to INDEX-DERIVED diagnostics only, no
// real compiler/build execution).
// ---------------------------------------------------------------------

// LSP 3.17 `DiagnosticSeverity`.
constexpr int kDiagnosticSeverityError = 1;
constexpr int kDiagnosticSeverityWarning = 2;

Json DiagnosticFor(int start_line, std::size_t start_char, int end_line, std::size_t end_char, int severity,
                    std::string message) {
    return Json{
        {"range",
         Json{
             {"start", Json{{"line", start_line}, {"character", start_char}}},
             {"end", Json{{"line", end_line}, {"character", end_char}}},
         }},
        {"severity", severity},
        {"source", "aistudio-index"},
        {"message", std::move(message)},
    };
}

// One parse-error span, in tree-sitter's own TSPoint terms (row is
// 0-based like LSP's own Position.line; column is a UTF-8 BYTE offset
// within that row, same as every other tree-sitter position this
// codebase already converts via Utf8ByteToUtf16Offset -- see
// AstExtractor.cpp's start_line/end_line for the same row convention).
struct ParseErrorSpan {
    TSPoint start;
    TSPoint end;
};

// Depth-first, source-order search for the first ERROR node
// (tree-sitter's own node kind for text it couldn't parse into any real
// grammar rule) or MISSING token (a zero-width node tree-sitter's error
// recovery synthesized to stand in for something the grammar required
// but didn't find, e.g. a missing ';'). Only recurses into a child when
// ts_node_has_error() says that CHILD's own subtree contains an error --
// an error-free subtree is skipped in O(1) rather than walked in full,
// the same pruning tree-sitter's own docs recommend for this exact
// query. Returns the first (by source position) match; v1 surfaces only
// one parse-failure diagnostic per file rather than enumerating every
// error location, matching this feature's own deliberately narrow scope
// (docs/ROADMAP.md's Diagnostics Design Proposal, "index-derived only").
std::optional<ParseErrorSpan> FindFirstParseError(TSNode node) {
    if (ts_node_is_error(node) || ts_node_is_missing(node)) {
        return ParseErrorSpan{ts_node_start_point(node), ts_node_end_point(node)};
    }
    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        const TSNode child = ts_node_child(node, i);
        if (ts_node_has_error(child)) {
            if (const auto found = FindFirstParseError(child); found.has_value()) {
                return found;
            }
        }
    }
    return std::nullopt;
}

// Parses `content` with the same tree-sitter-cpp grammar
// SymbolExtractor/AstExtractor/IncludeExtractor already use, purely to
// answer "did this fail to parse cleanly".
//
// INVESTIGATION (docs/ROADMAP.md's Diagnostics Design Proposal claimed
// "AstExtractor/SymbolExtractor silently swallow parse failures" --
// re-verified here by reading both files in full): neither one records
// ANYTHING queryable about a parse failure -- not a return value, not an
// error list, not even a log line. Both build their own result (an
// AstNode tree / a std::vector<Symbol>) directly from tree-sitter's
// tolerant parse and simply never look at whether it succeeded cleanly.
// tree-sitter itself is deliberately error-tolerant -- a syntactically
// broken file still produces *a* tree, never a null/thrown failure -- so
// ts_node_has_error() (true if the root or any descendant is an ERROR
// node or a MISSING token) is the only "did this fail" signal it exposes
// at all.
//
// This re-parses the open document's text locally rather than adding a
// queryable failure signal to AstExtractor/SymbolExtractor themselves
// (the Design Proposal's own suggested alternative) for two reasons: (a)
// diagnostics here are computed once per didOpen/didChange for exactly
// the ONE currently-open document, never batched across the whole
// project the way SymbolIndex/IncludeGraph's own Build() passes are, so
// there is no shared per-file cache this could hook into instead without
// adding one; (b) ts_node_has_error() is a single O(tree size) walk, the
// same cost class as the JSON (de)serialization already happening on
// this exact round trip -- re-parsing one open file on every
// didChange is not the "new subsystem" scale of cost the Design Proposal
// was worried about for real compiler diagnostics.
std::optional<ParseErrorSpan> DetectParseError(const std::string& content) {
    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        return std::nullopt;
    }
    ts_parser_set_language(parser, tree_sitter_cpp());

    std::optional<ParseErrorSpan> result;
    TSTree* tree = ts_parser_parse_string(parser, nullptr, content.c_str(), static_cast<uint32_t>(content.size()));
    if (tree != nullptr) {
        const TSNode root = ts_tree_root_node(tree);
        if (ts_node_has_error(root)) {
            result = FindFirstParseError(root);
        }
        ts_tree_delete(tree);
    }
    ts_parser_delete(parser);
    return result;
}

// Computes every index-derived diagnostic for exactly one document
// (`relative_path`, already firewall-checked by the caller --
// PublishDiagnosticsIfChanged/ClearDiagnosticsOnClose below) against its
// current text `content` (the live didOpen/didChange buffer, NOT a
// re-read from disk -- this is the one capability in this file where the
// live buffer IS the authority on the whole file's diagnosis, unlike
// LocationFor's own disk-read for looking up a DIFFERENT symbol's
// declaration).
Json ComputeDiagnostics(const LspServerOptions& options, const std::string& relative_path,
                         const std::string& content) {
    Json diagnostics = Json::array();

    if (const auto parse_error = DetectParseError(content); parse_error.has_value()) {
        const std::string start_line_text = GetLine(content, static_cast<int>(parse_error->start.row));
        const std::string end_line_text = GetLine(content, static_cast<int>(parse_error->end.row));
        diagnostics.push_back(DiagnosticFor(
            static_cast<int>(parse_error->start.row), Utf8ByteToUtf16Offset(start_line_text, parse_error->start.column),
            static_cast<int>(parse_error->end.row), Utf8ByteToUtf16Offset(end_line_text, parse_error->end.column),
            kDiagnosticSeverityError,
            "Failed to parse this file as C++ (tree-sitter-cpp found a syntax error at or near this "
            "location); this project's index-derived features (symbols/definition/references/etc.) may be "
            "incomplete or stale for this file until the error is fixed."));
    }

    // Unresolved `#include "..."` -- Severity::Warning, not Error: see
    // LspServer.hpp's own comment on why (IncludeGraph's suffix-matching
    // resolution is a heuristic, not a real compiler include-search-path
    // simulation, so it can have false positives). `<...>` system
    // includes are excluded via `edge.is_system` -- IncludeGraph::Build()
    // never even attempts to resolve those (its own "if (!edge.is_system)"
    // gate in IncludeGraph.cpp), so an unresolved system include is
    // IncludeGraph's own by-design behavior, not a defect to surface.
    //
    // KNOWN LIMITATION, CONFIRMED BY REAL-MACHINE VERIFICATION (this
    // task's own aistudio_core_cli.exe --lsp run): unlike the parse-error
    // half above (which re-parses the live buffer fresh on every call),
    // this loop is driven entirely by `options.include_graph`'s edges,
    // built once at LSP-server startup (RunLspMode() in main.cpp) and
    // never refreshed by didChange. This is a STRONGER staleness gap
    // than the usual "point-in-time index" caveat LocationFor's own
    // comment documents elsewhere in this file: it's not just that a
    // NEWLY-added file on disk won't be reflected -- editing this exact
    // open document to remove the very `#include` line that caused the
    // warning does NOT clear it, because the edge being matched here
    // still exists in the untouched IncludeGraph regardless of what the
    // live buffer now says (only the RANGE recomputation below reacts to
    // the live buffer, falling back to a zero-width range once the
    // include text can no longer be found on that line -- the diagnostic
    // ITSELF persists until the file is saved and IncludeGraph is
    // rebuilt, which nothing in this process currently triggers; no
    // FileWatcher/didSave wiring exists yet for LSP mode). Documented
    // here, in LspServer.hpp, and in docs/ROADMAP.md rather than solved
    // in this task -- fixing it needs either didSave -> IncludeGraph::
    // UpdateFile() wiring (this server doesn't advertise or handle
    // textDocument/didSave at all today) or full FileWatcher integration,
    // both bigger than this task's index-derived-diagnostics-only scope.
    if (options.include_graph != nullptr) {
        for (const auto& edge : options.include_graph->AllEdges()) {
            if (edge.from_file != relative_path || edge.is_system || !edge.resolved_path.empty()) {
                continue;
            }
            const int line0 = edge.line > 0 ? edge.line - 1 : 0;
            const std::string line_text = GetLine(content, line0);
            std::size_t start_char = 0;
            std::size_t end_char = 0;
            // Plain substring search (not FindWholeWord -- include_text
            // can itself contain '/' and '.', neither of which is an
            // identifier character) for the include's own path text
            // within its directive's line. Falls back to a zero-width
            // range at the line's start if not found (e.g. IncludeGraph
            // was built from on-disk content that has since diverged
            // from this live buffer -- the same "SymbolIndex/IncludeGraph
            // is point-in-time" limitation LocationFor's own comment
            // already documents elsewhere in this file), rather than
            // guessing.
            if (const auto byte_pos = line_text.find(edge.include_text); byte_pos != std::string::npos) {
                start_char = Utf8ByteToUtf16Offset(line_text, byte_pos);
                end_char = Utf8ByteToUtf16Offset(line_text, byte_pos + edge.include_text.size());
            }
            diagnostics.push_back(DiagnosticFor(
                line0, start_char, line0, end_char, kDiagnosticSeverityWarning,
                "Unresolved #include \"" + edge.include_text +
                    "\" -- no project file found under project root whose path ends in this text. This may "
                    "be a false positive: resolution matches the shortest scanned project file path ending in "
                    "this include text, not the compiler's actual include-search-path configuration, so a "
                    "header reachable only via a non-project or not-yet-scanned search path will show here "
                    "even though a real compiler build would find it."));
        }
    }

    return diagnostics;
}

// Resolves `uri` to a firewall-checked, project-relative path -- the
// shared precondition PublishDiagnosticsIfChanged and
// ClearDiagnosticsOnClose both need before touching `published_nonempty`
// (a firewalled or unresolvable URI never has an entry in that map at
// all, on either code path, so the two stay consistent with each other).
std::optional<std::string> DiagnosableRelativePath(const LspServerOptions& options, const std::string& uri) {
    if (options.project_root.empty()) {
        return std::nullopt;
    }
    auto relative_path = UriToProjectRelativePath(uri, options.project_root);
    if (!relative_path.has_value()) {
        return std::nullopt;
    }
    if (options.firewall != nullptr && !options.firewall->IsAllowed(*relative_path)) {
        return std::nullopt;
    }
    return relative_path;
}

// Computes diagnostics for `uri`'s current buffer content and publishes
// them via `out` -- but ONLY when doing so would change what the client
// currently sees: either the new list is non-empty, or the previous
// publish for this same `uri` was (meaning this call needs to clear it).
// A quiet transition from "no diagnostics" to "still no diagnostics" (the
// overwhelming common case -- most didOpen/didChange calls are on files
// with neither a parse error nor an unresolved include) sends nothing at
// all. This is a deliberate v1 design choice, not just an optimization:
// it keeps this server's existing request/response tests (every one of
// which opens well-formed, complete C++ snippets with no #include
// directives at all) byte-for-byte unaffected by this feature's
// addition -- see docs/ROADMAP.md's Diagnostics entry for the full
// reasoning and the alternative ("always publish, even empty, on every
// didOpen/didChange") this was weighed against. `published_nonempty` is
// Run()'s own per-connection state (parallel to `documents`), keyed by
// URI, tracking only the one bit needed to detect this transition.
void PublishDiagnosticsIfChanged(const LspServerOptions& options, const std::string& uri,
                                  const DocumentStore& documents, std::map<std::string, bool>& published_nonempty,
                                  std::ostream& out) {
    const auto relative_path = DiagnosableRelativePath(options, uri);
    if (!relative_path.has_value()) {
        return;
    }
    const auto doc_it = documents.find(uri);
    if (doc_it == documents.end()) {
        return; // shouldn't happen right after didOpen/didChange, but nothing to diagnose without buffer text
    }

    Json diagnostics = ComputeDiagnostics(options, *relative_path, doc_it->second);
    const bool nonempty = !diagnostics.empty();
    const auto it = published_nonempty.find(uri);
    const bool was_nonempty = it != published_nonempty.end() && it->second;
    if (!nonempty && !was_nonempty) {
        return; // quiet no-op transition -- see this function's own comment
    }
    published_nonempty[uri] = nonempty;
    WriteMessage(out, MakeNotification("textDocument/publishDiagnostics",
                                        Json{{"uri", uri}, {"diagnostics", std::move(diagnostics)}}));
}

// Clears a document's diagnostics on textDocument/didClose -- but again
// only when there's actually something to clear (see
// PublishDiagnosticsIfChanged's own comment on why silence is preferred
// over noise for the common "never had any diagnostics" case). Takes no
// LspServerOptions -- unlike PublishDiagnosticsIfChanged, nothing here
// needs to re-derive relative_path/firewall status: `published_nonempty`
// only ever gained an entry for `uri` in the first place via a prior
// PublishDiagnosticsIfChanged call that already passed both checks (see
// DiagnosableRelativePath), so a firewalled or unresolvable URI is
// simply never present in the map to begin with.
void ClearDiagnosticsOnClose(const std::string& uri, std::map<std::string, bool>& published_nonempty,
                              std::ostream& out) {
    const auto it = published_nonempty.find(uri);
    const bool was_nonempty = it != published_nonempty.end() && it->second;
    published_nonempty.erase(uri);
    if (!was_nonempty) {
        return;
    }
    WriteMessage(out, MakeNotification("textDocument/publishDiagnostics",
                                        Json{{"uri", uri}, {"diagnostics", Json::array()}}));
}

// Extracts `params.textDocument.uri`, the one field every
// didOpen/didChange/didClose notification's params share -- used by
// Run() below to know which document to (re)compute diagnostics for
// right after HandleDidOpen/HandleDidChange/HandleDidClose has already
// validated and applied the same params.
std::optional<std::string> DocumentUriFromParams(const Json& params) {
    if (!params.contains("textDocument")) {
        return std::nullopt;
    }
    const auto& text_document = params["textDocument"];
    if (!text_document.contains("uri") || !text_document["uri"].is_string()) {
        return std::nullopt;
    }
    return text_document["uri"].get<std::string>();
}

} // namespace

void LspServer::Run(std::istream& in, std::ostream& out) const {
    DocumentStore documents;
    bool shutdown_received = false;
    // textDocument/publishDiagnostics's own per-connection state -- see
    // PublishDiagnosticsIfChanged's own comment on why only this one bit
    // (not the full last-published diagnostics list) needs to persist
    // across notifications.
    std::map<std::string, bool> diagnostics_published_nonempty;

    while (true) {
        std::string body;
        if (!ReadMessage(in, body)) {
            break; // EOF or malformed framing
        }

        Json request;
        try {
            request = Json::parse(body);
        } catch (const std::exception&) {
            WriteMessage(out, MakeError(nullptr, -32700, "Parse error"));
            continue;
        }
        if (!request.is_object()) {
            WriteMessage(out, MakeError(nullptr, -32600, "Invalid Request"));
            continue;
        }

        const bool has_id = request.contains("id") && !request["id"].is_null();
        const Json id = has_id ? request["id"] : Json(nullptr);
        const auto method = request.value("method", std::string());
        const auto params = request.value("params", Json::object());

        try {
            if (!has_id) {
                // Notification -- MUST NOT receive a response (JSON-RPC 2.0).
                if (method == "exit") {
                    break; // LSP lifecycle end -- see Run()'s own doc comment
                }
                if (method == "textDocument/didOpen") {
                    HandleDidOpen(params, documents);
                    if (const auto uri = DocumentUriFromParams(params); uri.has_value()) {
                        PublishDiagnosticsIfChanged(options_, *uri, documents, diagnostics_published_nonempty, out);
                    }
                } else if (method == "textDocument/didChange") {
                    HandleDidChange(params, documents);
                    if (const auto uri = DocumentUriFromParams(params); uri.has_value()) {
                        PublishDiagnosticsIfChanged(options_, *uri, documents, diagnostics_published_nonempty, out);
                    }
                } else if (method == "textDocument/didClose") {
                    HandleDidClose(params, documents);
                    if (const auto uri = DocumentUriFromParams(params); uri.has_value()) {
                        ClearDiagnosticsOnClose(*uri, diagnostics_published_nonempty, out);
                    }
                }
                // "initialized" and any other unrecognized notification: no-op.
                continue;
            }

            if (shutdown_received) {
                // Per the LSP spec, every request after "shutdown" (other
                // than the id-less "exit" notification handled above)
                // MUST be rejected with InvalidRequest.
                WriteMessage(out, MakeError(id, -32600, "Invalid Request: shutdown already requested"));
            } else if (method == "initialize") {
                WriteMessage(out, MakeResult(id, InitializeResult(options_)));
            } else if (method == "shutdown") {
                shutdown_received = true;
                WriteMessage(out, MakeResult(id, Json(nullptr)));
            } else if (method == "textDocument/definition") {
                WriteMessage(out, MakeResult(id, HandleDefinition(options_, documents, params)));
            } else if (method == "textDocument/typeDefinition") {
                WriteMessage(out, MakeResult(id, HandleTypeDefinition(options_, documents, params)));
            } else if (method == "textDocument/documentSymbol") {
                WriteMessage(out, MakeResult(id, HandleDocumentSymbol(options_, params)));
            } else if (method == "workspace/symbol") {
                WriteMessage(out, MakeResult(id, HandleWorkspaceSymbol(options_, params)));
            } else if (method == "textDocument/references") {
                WriteMessage(out, MakeResult(id, HandleReferences(options_, documents, params)));
            } else if (method == "textDocument/rename") {
                WriteMessage(out, HandleRename(options_, documents, id, params));
            } else if (method == "textDocument/prepareRename") {
                WriteMessage(out, HandlePrepareRename(options_, documents, id, params));
            } else {
                WriteMessage(out, MakeError(id, -32601, "Method not found: " + method));
            }
        } catch (const std::exception& e) {
            WriteMessage(out, MakeError(id, -32600, std::string("Invalid Request: ") + e.what()));
        }
    }
}

} // namespace aistudio::core
