#pragma once

#include <string>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "固定ファイル名のIDE状態" (full resolution):
// derives a short, stable identifier for a workspace root, used to give
// each project/workspace its own EditorStateStore state file instead of
// every IDE window fighting over one fixed name
// (%TEMP%/aistudio_editor_state.json).
//
// This exact algorithm is mirrored in three other languages that must
// produce byte-identical output for the same real directory --
// Tools/vscode-extension/src/editorState.ts, Tools/visualstudio-
// extension/AiStudioCoreLsp/EditorStateWriter.cs, Tools/rider-plugin/
// .../EditorStateWriter.kt -- so changing it here requires updating all
// three. The algorithm, spelled out once (this is the reference copy):
//
//   1. Resolve `workspace_root` to an absolute, weakly-canonical path
//      (this side only -- Core/src/main.cpp's `project.root` config
//      defaults to "." when unset, so this step is what turns that into
//      an absolute path comparable to what an IDE extension reports; the
//      three IDE-side implementations already receive an absolute path
//      from their own workspace-root API and skip this step).
//   2. Normalize the resulting string: '\' -> '/', strip any trailing
//      '/', lowercase ASCII letters only (A-Z -> a-z; every other byte,
//      including UTF-8 continuation bytes, is left untouched -- this is
//      NOT full Unicode case-folding, a deliberate simplification since
//      Windows paths are the only target this ships for, see CLAUDE.md).
//   3. Hash the UTF-8 bytes of the normalized string with 64-bit FNV-1a
//      (offset basis 0xcbf29ce484222325, prime 0x100000001b3).
//   4. Format the 64-bit result as 16 lowercase hex digits.
//
// Known limitation, accepted rather than solved: if the two sides'
// underlying strings don't converge to the identical normalized form
// (e.g. a symlink/junction Core's weakly_canonical resolves but the IDE's
// own path API reports unresolved), the hashes won't match and
// EditorStateStore::Read() simply finds no file for this workspace --
// the same "no known editor state" fallback Read() already uses for
// every other unreadable/inapplicable case, not a new failure mode.
//
// Returns empty string if `workspace_root` is empty, or fails to
// resolve -- callers fall back to the pre-existing fixed filename in
// that case (see EditorStateStore::DefaultStateFilePath()).
[[nodiscard]] std::string WorkspaceHash(const std::string& workspace_root);

} // namespace aistudio::core
