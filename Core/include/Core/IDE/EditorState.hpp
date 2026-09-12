#pragma once

#include <optional>
#include <string>

namespace aistudio::core {

// 0-based line/character positions, matching vscode.Position (and this
// project's own LSP layer's Position type, Core/LSP/LspServer.cpp) --
// NOT the 1-based Symbol::line convention SymbolIndex/AstIndex/the other
// project-intelligence indexes use. Deliberately UTF-16-code-unit based
// (whatever the reporting IDE's own native column unit is), same as LSP
// Position -- this is a passthrough of what the IDE extension reports,
// not a value this project computes itself.
struct EditorSelection {
    int start_line = 0;
    int start_character = 0;
    int end_line = 0;
    int end_character = 0;
};

// What one connected IDE extension currently has open/focused and
// selected -- docs/ROADMAP.md "## IDE" Active document / Selection
// checklist items. Deliberately IDE-agnostic (AGENT.md #2: don't embed
// VS Code-specific logic into Core): `source` records which IDE
// extension reported it, but nothing about the shape itself assumes VS
// Code -- a future Visual Studio/Rider plugin can report through this
// same schema (see EditorStateStore.hpp's own comment on the file this
// is read from).
struct EditorState {
    // Which IDE extension wrote this state, e.g. "vscode". Empty if the
    // writer didn't set one. Informational only -- EditorStateStore
    // itself never branches on it.
    std::string source;
    // Project-relative path (converted from whatever absolute path the
    // IDE extension reported, via EditorStateStore::Options::project_root)
    // when a project_root was configured; the original path exactly as
    // written otherwise. nullopt when no file is currently focused in the
    // IDE (no editor open, or focus is on something that isn't a plain
    // file -- an untitled buffer, a diff view, a settings page) -- this
    // is a real, reportable state in its own right, distinct from
    // EditorStateStore::Read() itself returning nullopt for "no usable
    // state available at all" (state file missing/malformed, or the
    // reported path was rejected by the Context Firewall).
    std::optional<std::string> active_document_path;
    // nullopt whenever active_document_path is nullopt, or when the IDE
    // reports an empty (cursor-only, nothing highlighted) selection --
    // "if any", per docs/ROADMAP.md's Selection checklist item wording.
    std::optional<EditorSelection> selection;
    // Raw ISO-8601 UTC timestamp string as the IDE extension wrote it,
    // unparsed -- kept for a future staleness check (e.g. "this state is
    // from an IDE session that's no longer running"), which this slice
    // deliberately doesn't implement yet (see EditorStateStore.hpp).
    std::string updated_at;
};

} // namespace aistudio::core
