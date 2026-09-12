#include "Core/IDE/EditorStateStore.hpp"

#include "Core/Util/Utf8.hpp"
#include "Core/Util/WorkspaceHash.hpp"

// Vendored header-only third-party library (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since this header isn't ours to fix --
// same pattern as Core/src/API/ApiServer.cpp / Core/src/MCP/McpServer.cpp.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>
#include <sstream>

namespace aistudio::core {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr int kSupportedSchemaVersion = 1;

// Converts an absolute (or already-relative) filesystem path into a
// path relative to `project_root`, the same "weakly_canonical both
// sides, then lexically_relative, reject anything starting with '..'"
// approach Core/src/LSP/LspServer.cpp's UriToProjectRelativePath already
// uses for the equivalent URI->path conversion -- duplicated here rather
// than shared, since that function lives in LspServer.cpp's own
// anonymous namespace and starts from a `file://` URI, not a plain path;
// extracting a shared helper for two call sites isn't worth it yet
// (AGENT.md #14 -- if a third consumer needs this, extract then).
// Returns nullopt if either path fails to resolve, or if `path` isn't
// actually inside `project_root`.
std::optional<std::string> ToProjectRelativePath(const std::string& path, const std::string& project_root) {
    std::error_code target_ec;
    std::error_code root_ec;
    // Utf8ToPath(), not fs::path(narrow_string) directly -- confirmed via
    // a real crash (docs/ROADMAP.md "非ASCIIパスでEditorStateStoreが
    // クラッシュ") that fs::path's own narrow-string constructor throws
    // for a non-ASCII `path`/`project_root` on Windows (CP_ACP round-trip,
    // not UTF-8 -- see Utf8ToPath's own doc comment).
    const auto target = fs::weakly_canonical(Utf8ToPath(path), target_ec);
    const auto root = fs::weakly_canonical(Utf8ToPath(project_root), root_ec);
    if (target_ec || root_ec) {
        return std::nullopt;
    }

    const auto relative = target.lexically_relative(root);
    const std::string relative_str = PathToUtf8Generic(relative);
    if (relative.empty() || relative_str.rfind("..", 0) == 0) {
        return std::nullopt; // outside project_root
    }
    return relative_str;
}

// Closes a gap the Context Firewall alone doesn't cover (docs/ROADMAP.md
// Phase 13 "固定ファイル名のIDE状態"): the firewall only rejects an
// active_document_path OUTSIDE its own root, so a second IDE window with
// a DIFFERENT workspace root that happens to be nested inside (or a
// sibling sharing a path prefix with) this project's root -- e.g. a
// subproject in a monorepo -- would pass the firewall's containment
// check and silently bias this project's context toward a file some
// other project's editor has open. Tools/vscode-extension/src/
// editorState.ts (and the Visual Studio/Rider counterparts) already
// write an unused `workspace_root` field into every snapshot for
// exactly this future use (see their own comments) -- this compares it
// against this process's own `project_root` and, on a confirmed
// mismatch, treats the whole file the same as "no known editor state"
// (nullopt), consistent with every other malformed/inapplicable case
// Read() already collapses to nullopt.
//
// Deliberately fails OPEN (returns false / "don't drop") when either
// side is missing or fails to resolve: `workspace_root` is an older-
// writer-optional field and `project_root` itself is optional
// (EditorStateStore::Options), and this check is a precision
// improvement on top of the firewall, not a replacement for it -- the
// firewall's own path-containment check remains the actual security
// boundary. Does NOT solve the separate, still-open "last writer wins"
// problem for two IDE windows on entirely unrelated projects sharing
// this same fixed state file (that needs per-workspace file naming,
// which -- unlike this check -- requires the IDE extensions themselves
// to change what path they write to).
bool WorkspaceRootMismatch(const Json& doc, const std::string& project_root) {
    if (project_root.empty()) {
        return false;
    }
    if (!doc.contains("workspace_root") || !doc["workspace_root"].is_string()) {
        return false;
    }
    const std::string reported = doc["workspace_root"].get<std::string>();
    if (reported.empty()) {
        return false;
    }

    std::error_code reported_ec;
    std::error_code root_ec;
    const auto reported_canonical = fs::weakly_canonical(Utf8ToPath(reported), reported_ec);
    const auto root_canonical = fs::weakly_canonical(Utf8ToPath(project_root), root_ec);
    if (reported_ec || root_ec) {
        return false;
    }
    return reported_canonical != root_canonical;
}

} // namespace

EditorStateStore::EditorStateStore(Options options) : options_(std::move(options)) {
    if (options_.state_file_path.empty()) {
        options_.state_file_path = DefaultStateFilePath(options_.project_root);
    }
}

std::string EditorStateStore::DefaultStateFilePath(const std::string& workspace_root) {
    const std::string hash = WorkspaceHash(workspace_root);
    const std::string file_name = hash.empty() ? "aistudio_editor_state.json" : "aistudio_editor_state_" + hash + ".json";
    return PathToUtf8(fs::temp_directory_path() / file_name);
}

std::optional<EditorState> EditorStateStore::Read() const {
    std::ifstream in(Utf8ToPath(options_.state_file_path), std::ios::binary);
    if (!in) {
        return std::nullopt; // not running / hasn't written yet -- normal
    }

    std::stringstream buffer;
    buffer << in.rdbuf();

    Json doc;
    try {
        doc = Json::parse(buffer.str());
    } catch (const std::exception&) {
        return std::nullopt; // malformed JSON -- e.g. read mid-write
    }
    if (!doc.is_object()) {
        return std::nullopt;
    }

    if (!doc.contains("version") || !doc["version"].is_number_integer() ||
        doc["version"].get<int>() != kSupportedSchemaVersion) {
        return std::nullopt; // missing/unrecognized schema version
    }
    if (!doc.contains("active_document_path") || !doc.contains("selection")) {
        return std::nullopt; // malformed -- required keys missing
    }
    if (WorkspaceRootMismatch(doc, options_.project_root)) {
        return std::nullopt; // state belongs to a different IDE workspace
    }

    EditorState state;
    state.source = doc.value("source", std::string());
    state.updated_at = doc.value("updated_at", std::string());

    const auto& path_field = doc["active_document_path"];
    if (path_field.is_string() && !path_field.get<std::string>().empty()) {
        auto path = path_field.get<std::string>();

        // Context Firewall (docs/MASTER_SPEC.md #21): a path outside the
        // sandbox root is dropped silently, the same way every other MCP
        // tool result's own firewall filtering in McpServer.cpp drops
        // disallowed matches rather than erroring the whole call.
        if (options_.firewall == nullptr || options_.firewall->IsAllowed(path)) {
            if (!options_.project_root.empty()) {
                if (auto relative = ToProjectRelativePath(path, options_.project_root); relative.has_value()) {
                    path = *relative;
                }
                // Conversion failure without project_root disagreeing
                // with an already-passed firewall check just means
                // project_root/firewall don't share a root -- fall back
                // to the path exactly as reported rather than dropping it,
                // since the firewall (the actual security boundary, when
                // configured) already accepted it.
            }
            state.active_document_path = std::move(path);
        }
    }

    if (state.active_document_path.has_value()) {
        const auto& selection_field = doc["selection"];
        if (selection_field.is_object() && selection_field.contains("start_line") &&
            selection_field.contains("start_character") && selection_field.contains("end_line") &&
            selection_field.contains("end_character")) {
            EditorSelection selection;
            selection.start_line = selection_field.value("start_line", 0);
            selection.start_character = selection_field.value("start_character", 0);
            selection.end_line = selection_field.value("end_line", 0);
            selection.end_character = selection_field.value("end_character", 0);
            state.selection = selection;
        }
    }

    return state;
}

} // namespace aistudio::core
