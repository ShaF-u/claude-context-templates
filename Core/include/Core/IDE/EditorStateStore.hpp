#pragma once

#include "Core/IDE/EditorState.hpp"
#include "Core/Security/Sandbox.hpp"

#include <optional>
#include <string>

namespace aistudio::core {

// Reads the small JSON side-channel file a connected IDE extension
// (Tools/vscode-extension/src/editorState.ts today; any future Visual
// Studio/Rider counterpart is meant to write the exact same schema)
// writes on every active-editor/selection change -- docs/ROADMAP.md
// "## IDE" Active document / Selection checklist items.
//
// Why a file instead of a new server endpoint (investigated first, per
// AGENT.md #1, before choosing this): Core/src/main.cpp's `--lsp` /
// `--mcp` / default (HTTP ApiServer) modes are three mutually exclusive
// branches inside main() -- each one `return`s before the next is even
// considered. An IDE extension connected to `--lsp` for LanguageClient
// features (Tools/vscode-extension/src/extension.ts) is therefore
// talking to a Core process that has no HTTP ApiServer and no MCP
// stdio server running alongside it to push state to. Adding "run every
// mode in one process" support would be a much larger, separate change
// (a new kind of multi-server bootstrap, not a small addition to an
// existing one) -- well beyond this task's scope (AGENT.md #14). Even
// if that existed, the process an IDE extension is connected to and the
// process that eventually wants this (McpServer.cpp's `active_document`
// tool, today's only real consumer, runs inside `--mcp` mode -- a
// SEPARATE process from `--lsp`) aren't guaranteed to be the same
// process or even running at the same time. A small polled file is the
// smallest integration that actually crosses that boundary -- the same
// "OS temp directory, no handshake needed" convention
// Core/src/main.cpp's RunLspMode()/RunMcpMode() use for their own log
// files (aistudio_lsp_<pid>.log / aistudio_mcp_<pid>.log, one per
// process since docs/ROADMAP.md Phase 13's "固定ファイル名のログ" fix) --
// but this file, unlike those, stays a single well-known filename
// (see DefaultStateFilePath()): if more than one IDE window is
// reporting state for a different project at the same time, the last
// writer wins -- this process may simply see no editor state at all
// for stretches of time when some other project's IDE window last wrote
// the file. Still documented, not solved (docs/ROADMAP.md Phase 13's
// remaining "固定ファイル名のIDE状態" item; a real fix needs the IDE
// extensions themselves to write to a workspace-specific file name, a
// cross-language change out of scope here). What IS handled here: every
// extension already writes an (until now unused) `workspace_root` field
// into each snapshot specifically so Read() can reject a state file that
// belongs to a DIFFERENT workspace than this process's own
// Options::project_root -- see WorkspaceRootMismatch() in
// EditorStateStore.cpp. This matters even with a Sandbox firewall
// configured: the firewall only rejects a path OUTSIDE its root, so a
// nested/sibling project (e.g. a monorepo subproject) whose active file
// happens to fall INSIDE this root would otherwise pass the firewall's
// containment check and silently bias this project's context toward a
// file a different project's editor has open.
//
// One-way only: extension -> file -> EditorStateStore::Read(). Nothing
// in this class ever writes to the file.
//
// UPDATE (docs/ROADMAP.md Phase 13 "固定ファイル名のIDE状態", full
// resolution): the "single well-known filename" limitation described
// above is now only the FALLBACK behavior. DefaultStateFilePath(), when
// given a non-empty workspace root, derives a per-workspace filename via
// Core::WorkspaceHash (Core/include/Core/Util/WorkspaceHash.hpp) --
// aistudio_editor_state_<16 hex chars>.json instead of the fixed name --
// so two IDE windows on different projects no longer share one file.
// This requires the writer side (Tools/vscode-extension/src/
// editorState.ts, Tools/visualstudio-extension/.../EditorStateWriter.cs,
// Tools/rider-plugin/.../EditorStateWriter.kt) to compute the exact same
// hash from their own reported workspace_root -- see WorkspaceHash.hpp's
// doc comment for the full cross-language algorithm spec. The legacy
// fixed filename remains the fallback when no workspace root is known on
// either side (e.g. this class's own tests, which pass an empty
// Options::project_root).
class EditorStateStore {
public:
    struct Options {
        // Defaults to DefaultStateFilePath() when left empty (the
        // constructor fills it in) -- override only for tests, so they
        // never touch the real OS-temp-directory file another process
        // (a real IDE extension, or a real `--mcp` process) might also be
        // reading/writing.
        std::string state_file_path;
        // Used to convert the IDE-reported path into a project-relative
        // path, matching every other MCP tool's own file_path convention
        // (symbol_search, ast_tree, include_graph, ...). Empty (default)
        // leaves the path exactly as the IDE extension wrote it.
        std::string project_root;
        // Context Firewall (docs/MASTER_SPEC.md #21) -- an
        // active_document_path outside `firewall`'s root is treated the
        // same as "no active document" (silently dropped, matching every
        // other MCP tool result's own firewall filtering in
        // Core/src/MCP/McpServer.cpp) rather than surfaced as an error.
        // nullptr applies no filtering.
        const Sandbox* firewall = nullptr;
    };

    explicit EditorStateStore(Options options = {});

    // OS temp dir (matches RunLspMode()/RunMcpMode()'s own log-file
    // placement) plus a filename derived from `workspace_root` via
    // Core::WorkspaceHash -- or the legacy fixed filename
    // (aistudio_editor_state.json) when `workspace_root` is empty or
    // fails to resolve to a hash. See the class comment above for why
    // this exists and WorkspaceHash.hpp for the algorithm every writer
    // side must match exactly.
    [[nodiscard]] static std::string DefaultStateFilePath(const std::string& workspace_root = "");

    // Reads and parses the state file fresh on every call -- no caching,
    // since the underlying file changes on every keystroke/cursor move in
    // the IDE (a cache would only ever look stale; a call site that wants
    // fewer reads should decide that itself, the same open question
    // McpServerOptions::context_cache's own comment already raises for a
    // different tool).
    //
    // Returns nullopt when: the file doesn't exist (the IDE extension
    // isn't running, or hasn't written yet -- the expected steady state,
    // not an error), the JSON is malformed, "version" is missing or not
    // the one version this reads, or a required field is missing/has the
    // wrong type. Every failure mode collapses to "no known editor
    // state" rather than a thrown exception or a Result<Error> -- a
    // missing/unreadable file is normal here, not exceptional.
    [[nodiscard]] std::optional<EditorState> Read() const;

private:
    Options options_;
};

} // namespace aistudio::core
