#pragma once

#include "Core/Error/Result.hpp"

#include <string>

namespace aistudio::core {

// Writes a `.mcp.json` into `worktree_path` that launches
// `core_cli_absolute_path --mcp` as a stdio MCP server -- docs/ROADMAP.md
// Phase 13 "13-4. セッション単位のMCP接続・コンテキスト管理".
//
// A session's CLI-type AI (launched with the worktree as its own working
// directory -- see CliProfile::working_directory) spawns whatever this
// file's "command" points to as ITS OWN child process (standard MCP
// stdio transport: the client owns server process lifecycle, this
// Studio never launches "--mcp" processes itself). Core's own
// `project.root` config already defaults to "." when no aistudio.config
// overrides it (see Core/src/main.cpp's RunMcpMode()), so a --mcp
// process spawned with the worktree as its cwd is automatically scoped
// to that worktree alone -- its own Sandbox/ContextCache/SymbolIndex,
// entirely separate from every other session's. This function is the
// one missing piece to make that real: without a `.mcp.json` actually
// present in the worktree, the CLI has nothing telling it to launch a
// server at all.
//
// `core_cli_absolute_path` must be absolute, not the relative path the
// project's own root `.mcp.json` uses -- a worktree doesn't have its own
// `build/` output (builds aren't per-worktree), so a copied-verbatim
// relative path would resolve to a nonexistent file there.
Result<void> WriteWorktreeMcpConfig(const std::string& worktree_path, const std::string& core_cli_absolute_path);

} // namespace aistudio::core
