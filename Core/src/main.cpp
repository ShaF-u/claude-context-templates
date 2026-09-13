#include "Core/API/ApiServer.hpp"
#include "Core/Analysis/ImpactAnalyzer.hpp"
#include "Core/Backend/BackendFactoryRegistry.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Config/Config.hpp"
#include "Core/Database/Database.hpp"
#include "Core/Database/Migrator.hpp"
#include "Core/Database/StringList.hpp"
#include "Core/Database/TaskRepository.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/IDE/EditorStateStore.hpp"
#include "Core/Index/AstIndex.hpp"
#include "Core/LLM/EchoLLMProvider.hpp"
#include "Core/Index/CallGraph.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/InheritanceGraph.hpp"
#include "Core/Index/ReferenceGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/Logging/Logger.hpp"
#include "Core/LSP/LspServer.hpp"
#include "Core/MCP/McpServer.hpp"
#include "Core/Plugin/PluginBootstrap.hpp"
#include "Core/Plugin/PluginPermissions.hpp"
#include "Core/Plugin/PluginRegistry.hpp"
#include "Core/Search/KeywordSearch.hpp"
#include "Core/Search/SymbolSearch.hpp"
#include "Core/Security/Sandbox.hpp"
#include "Core/Context/ContextAuditEntry.hpp"
#include "Core/Context/ContextSelector.hpp"
#include "Core/Context/ContextSnapshot.hpp"
#include "Core/Context/AstContextSource.hpp"
#include "Core/Context/ContextCache.hpp"
#include "Core/Context/ContextRetriever.hpp"
#include "Core/Context/SentLedger.hpp"
#include "Core/Context/DependencyContextSource.hpp"
#include "Core/Context/FileContextSource.hpp"
#include "Core/Database/ContextAuditRepository.hpp"
#include "Core/Database/ContextSnapshotRepository.hpp"
#include "Core/Project/FileCache.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Project/FileWatcher.hpp"
#include "Core/Util/Utf8.hpp"

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

#include <algorithm>
#include <any>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string_view>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace aistudio::core;

namespace {

// Placeholder Backend proving Register/Capability lookup works end-to-end.
// Real Backends (Unreal, Unity, Git, IDE, ...) replace this in later
// phases (see docs/ROADMAP.md Phase 1/6/7).
class NullBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "core.null"; }
    [[nodiscard]] std::string Name() const override { return "Null Backend"; }
    [[nodiscard]] std::string Version() const override { return "0.1.0"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override {
        // "@1.0.0" opts this capability into Capability System
        // versioning/negotiation (docs/ROADMAP.md) — most capabilities
        // stay unversioned (bare name), this is just to demonstrate a
        // versioned one end-to-end below.
        return {"diagnostics.ping@1.0.0", "diagnostics.delete"};
    }
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }

    CommandResult Dispatch(const Command& command) override {
        if (command.name == "diagnostics.ping") {
            return CommandResult::Ok(std::any(std::string("pong")));
        }
        // Matches PermissionPolicy's default "*.delete" dangerous pattern —
        // a live example of a Command that goes through ApiServer's
        // approval flow instead of dispatching immediately.
        if (command.name == "diagnostics.delete") {
            return CommandResult::Ok(std::any(std::string("logs cleared")));
        }
        return CommandResult::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "core.null does not support command: " + command.name,
            .module = "Backend.Null",
        });
    }

    QueryResult Handle(const Query&) override {
        return QueryResult::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "core.null does not support any queries yet",
            .module = "Backend.Null",
        });
    }

    Result<void> Configure(const Config& config) override {
        AISTUDIO_LOG_INFO("Backend.Null", "configured with studio.name='" + config.GetOr("studio.name", "") + "'");
        return Result<void>::Ok();
    }
};

AISTUDIO_REGISTER_BACKEND(NullBackend);

// Per-process log file name (docs/ROADMAP.md Phase 13, "固定ファイル名のログ"):
// two --mcp/--lsp processes against the same or different projects used to
// collide on the same OS-temp-directory log file, which looked like a
// context_retrieve hang under concurrent MCP clients (actually a Windows
// sharing-violation stall on the shared file handle). The process id is
// already a unique, no-setup-required identifier for "this session" --
// no new SessionId concept needed.
std::string PerProcessLogFileName(const std::string& base_name) {
#if defined(_WIN32)
    const int pid = _getpid();
#else
    const int pid = getpid();
#endif
    return base_name + "_" + std::to_string(pid) + ".log";
}

// `scan.extra_ignore_patterns` in aistudio.config (comma-separated, same
// format as plugin.ids -- see SplitStringList): appended to every
// Index::Build() call's own FileScanner, on top of FileScanner's compiled-
// in DefaultIgnorePatterns(). Exists so a host project can exclude paths
// specific to its own layout (e.g. this tool checked out as a submodule
// under a project-specific folder name) without that name being baked
// into this template's generic defaults.
std::vector<std::string> LoadExtraIgnorePatterns(const Config& config) {
    return SplitStringList(config.GetOr("scan.extra_ignore_patterns", ""));
}

// MCP stdio mode (docs/MASTER_SPEC.md #63, CLAUDE.md's 2026-09-01
// direction correction): a deliberately separate, quiet bootstrap path
// rather than a branch threaded through the verbose HTTP bootstrap
// below. Two reasons: (1) the stdio JSON-RPC transport requires stdout
// to carry nothing but protocol messages, so none of the demo
// std::cout writes below are safe to run first, and (2) MCP tool
// serving only needs Config/Sandbox/SymbolIndex/IncludeGraph/
// ContextRetriever/a minimal BackendRegistry — the Database/
// PluginRegistry/EchoLLMProvider demo sequence below exists to prove
// those subsystems work, not because McpServer depends on them. The
// BackendRegistry built below (native Backends only, discovered via
// BackendFactoryRegistry -- no Plugin DLL loading) exists specifically
// so context_retrieve's Backend Context Provider retrieval (File/Git
// Provider, docs/ROADMAP.md "Context Provider SDK") has something to
// consult; it was deliberately left out of this mode until now.
int RunMcpMode() {
    // Console output disabled (see Logger::SetConsoleEnabled) so nothing
    // but McpServer's own JSON-RPC lines reaches stdout; SetLogFile keeps
    // this observable (AGENT.md #9) without violating that.
    Logger::Instance().SetMinLevel(LogLevel::Info);
    // Outside project.root, deliberately -- a relative "aistudio_mcp.log"
    // lands wherever this process's CWD happens to be, which for a
    // real MCP client (e.g. Claude Code, launched via .mcp.json at the
    // repo root) is typically the project root itself. Discovered live:
    // once GitBackend::Dispatch's git.commit ("git add -A") existed,
    // that put the log file under Git's control, and a later git.stash
    // then failed outright (git tried to unlink/restore a file this very
    // process still had open -- a Windows sharing-violation). Writing to
    // the OS temp directory instead means the log is never part of any
    // repository GitBackend operates on. The pid suffix (PerProcessLogFileName)
    // is what actually fixes concurrent --mcp sessions colliding on one file.
    Logger::Instance().SetLogFile(PathToUtf8(std::filesystem::temp_directory_path() / PerProcessLogFileName("aistudio_mcp")));
    Logger::Instance().SetConsoleEnabled(false);
    AISTUDIO_LOG_INFO("Core.MCP", "AI Development Studio Core starting in MCP stdio mode");

    Config config;
    if (const auto load_result = config.LoadFromFile("aistudio.config"); !load_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "no config file found, using defaults: " + load_result.Err().message);
    }
    const auto project_root = config.GetOr("project.root", ".");
    const auto extra_ignore_patterns = LoadExtraIgnorePatterns(config);
    // Opt-in only (this task's design discussion, see docs/ROADMAP.md):
    // git_commit/git_branch/git_stash stay absent from tools/list unless
    // an operator explicitly sets this in aistudio.config -- the
    // connecting MCP client's own permission mode is the intended gate
    // on actually calling them, not a new server-side approval
    // mechanism. Has no effect on the HTTP
    // POST /api/backends/core.git/commands route, which stays always
    // reachable.
    const bool enable_git_write_commands = config.GetOr("mcp.enable_git_write_commands", "false") == "true";

    // Opt-in like enable_git_write_commands above: the response shape
    // stays unchanged until an operator sets this (docs/MASTER_SPEC.md
    // #99). Unparseable or negative = no budget.
    std::int64_t context_response_budget_tokens = 0;
    if (const auto budget_str = config.Get("mcp.context_response_budget_tokens")) {
        try {
            context_response_budget_tokens = std::stoll(*budget_str);
        } catch (const std::exception&) {
            AISTUDIO_LOG_WARN("Core.MCP", "mcp.context_response_budget_tokens is not a number: '" + *budget_str +
                                                "' (ignored, responses stay unbudgeted)");
        }
        if (context_response_budget_tokens < 0) {
            context_response_budget_tokens = 0;
        }
    }

    // Opt-in (docs/ROADMAP.md CE-4): default false preserves the exact
    // pre-existing response shape.
    const bool suppress_resent_content = config.GetOr("mcp.suppress_resent_content", "false") == "true";

    // Context Firewall (docs/MASTER_SPEC.md #21): applied both inside
    // ContextRetriever (for context_retrieve) and directly by McpServer
    // itself (for symbol_search/keyword_search, which bypass
    // ContextRetriever entirely) — see Core/MCP/McpServer.hpp.
    const Sandbox sandbox(project_root);

    // Minimal native-only BackendRegistry (see this function's own doc
    // comment above) -- no Plugin DLL loading, no Database, no
    // EchoLLMProvider demo. Errors are logged and skipped rather than
    // aborting startup, matching this function's existing "warn and
    // continue" style for every index build above/below.
    BackendRegistry registry;
    for (auto& discovered : BackendFactoryRegistry::Instance().CreateAll()) {
        if (const auto register_result = registry.Register(discovered); !register_result) {
            AISTUDIO_LOG_WARN("Core.MCP", "failed to register backend: " + register_result.Err().message);
        }
    }
    if (const auto configure_result = registry.ConfigureAll(config); !configure_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "backend configuration issues: " + configure_result.Err().message);
    }
    if (const auto start_result = registry.StartAll(); !start_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "backend start issues: " + start_result.Err().message);
    }

    SymbolIndex symbol_index;
    if (const auto index_result = symbol_index.Build(project_root, extra_ignore_patterns); !index_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "SymbolIndex build failed: " + index_result.Err().message);
    }

    IncludeGraph include_graph;
    if (const auto graph_result = include_graph.Build(project_root, extra_ignore_patterns); !graph_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "IncludeGraph build failed: " + graph_result.Err().message);
    }

    // Built for the call_graph/inheritance_graph/reference_graph/
    // impact_analysis MCP tools (docs/ROADMAP.md Next Priority "MCP
    // サーバーの拡張") -- ContextRetriever itself doesn't need these
    // three, only SymbolIndex/IncludeGraph.
    CallGraph call_graph;
    if (const auto graph_result = call_graph.Build(project_root, extra_ignore_patterns); !graph_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "CallGraph build failed: " + graph_result.Err().message);
    }
    InheritanceGraph inheritance_graph;
    if (const auto graph_result = inheritance_graph.Build(project_root, extra_ignore_patterns); !graph_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "InheritanceGraph build failed: " + graph_result.Err().message);
    }
    ReferenceGraph reference_graph;
    if (const auto graph_result = reference_graph.Build(project_root, extra_ignore_patterns); !graph_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "ReferenceGraph build failed: " + graph_result.Err().message);
    }
    // Built for the ast_tree MCP tool (docs/ROADMAP.md Next Priority "MCP
    // サーバーの拡張") -- the one Phase 3 index the earlier graph-tool pass
    // left unexposed even though ApiServer's GET /api/ast has had it since
    // Phase 3.
    AstIndex ast_index;
    if (const auto ast_result = ast_index.Build(project_root, extra_ignore_patterns); !ast_result) {
        AISTUDIO_LOG_WARN("Core.MCP", "AstIndex build failed: " + ast_result.Err().message);
    }
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);

    // active_document MCP tool (docs/ROADMAP.md "## IDE") -- reads the
    // small JSON side-channel file a connected IDE extension writes on
    // every active-editor/selection change. See EditorStateStore.hpp for
    // why this is a polled file rather than a push endpoint: this mode
    // and the IDE extension's own `--lsp` connection are separate
    // processes with no shared memory or existing transport between them.
    // Built before context_retriever below (not after, as it was before
    // Active File Bias existed) so it can also be handed to
    // ContextRetriever::Options::editor_state_store -- see that field's
    // own doc comment for why this is the same store, not a second one.
    const EditorStateStore editor_state_store(EditorStateStore::Options{
        .project_root = project_root,
        .firewall = &sandbox,
    });

    const ContextRetriever context_retriever(ContextRetriever::Options{
        .symbol_index = &symbol_index,
        .include_graph = &include_graph,
        .project_root = project_root,
        .firewall = &sandbox,
        .backend_registry = &registry,
        // Active File Bias (docs/ROADMAP.md "## IDE" -- the follow-up
        // CLAUDE.md's Active document/Selection entry left explicitly
        // out of scope): context_retrieve now ranks items from/near
        // whatever file the connected IDE extension reports as active
        // higher, when one is connected. No effect when no IDE extension
        // is running (EditorStateStore::Read() returns nullopt) -- exact
        // pre-existing behavior.
        .editor_state_store = &editor_state_store,
    });

    // Context Cache (docs/ROADMAP.md Phase 2 "Context Cache"): memoizes
    // ContextRetriever::Retrieve() results by intent for the
    // context_retrieve tool below. Declared before the FileWatcher
    // subscription lambda so that lambda can capture it and call
    // InvalidateAll() -- the exact same shape as the default HTTP mode's
    // own ContextCache/FileWatcher pairing further down in this file (see
    // that block's comments for the full invalidation-design rationale).
    // McpServerOptions::context_cache's own doc comment explains why this
    // stayed nullptr in this function until now: a cache with no
    // invalidation trigger is the anti-pattern AGENT.md #7 warns against,
    // and the FileWatcher constructed just below is that trigger.
    ContextCache context_cache;

    // Content-hash based (docs/ROADMAP.md CE-4), not FileWatcher-driven --
    // a changed file naturally produces different ContextItem content on
    // the next Retrieve(), which SentLedger::Check() already treats as
    // "not unchanged", so no "FileChanged" subscription is needed here
    // the way context_cache above needs one.
    SentLedger sent_ledger;

    // FileWatcher (docs/ROADMAP.md Next Priority #10): keeps
    // SymbolIndex/IncludeGraph/CallGraph/InheritanceGraph/ReferenceGraph/
    // AstIndex fresh for as long as this stdio session lives, instead of
    // staying frozen at whatever they looked like when this process
    // started -- this mode's previous behavior for the lifetime of a
    // long-running MCP client connection (e.g. an interactive Claude Code
    // session left open for hours). Mirrors the default HTTP mode's
    // identical FileWatcher block below as closely as possible, down to
    // the same "FileChanged" subscription shape and the same generic
    // apply() lambda covering all six indexes.
    FileWatcher file_watcher(project_root, FileScanner(FileScanner::MakeOptions(extra_ignore_patterns)));
    const auto file_watcher_subscription_id = EventBus::Instance().Subscribe(
        "FileChanged",
        [&symbol_index, &include_graph, &call_graph, &inheritance_graph, &reference_graph, &ast_index,
         &context_cache, &project_root](const std::any& payload) {
            const auto* change = std::any_cast<FileChangeEvent>(&payload);
            if (change == nullptr) {
                return;
            }
            // Every index below shares the same UpdateFile(root, path) /
            // RemoveFile(path) -> Result<void> shape, so one generic
            // lambda replaces what would otherwise be six near-identical
            // if/else + log blocks (same reasoning as the HTTP mode's
            // identical apply() lambda below).
            const auto apply = [&](const char* index_name, auto& index) {
                const auto result = change->kind == FileChangeKind::Deleted
                                         ? index.RemoveFile(change->path)
                                         : index.UpdateFile(project_root, change->path);
                if (!result) {
                    AISTUDIO_LOG_WARN("Core.FileWatcher", std::string(index_name) +
                                                                " incremental update failed for '" + change->path +
                                                                "': " + result.Err().message);
                }
            };
            apply("SymbolIndex", symbol_index);
            apply("IncludeGraph", include_graph);
            apply("CallGraph", call_graph);
            apply("InheritanceGraph", inheritance_graph);
            apply("ReferenceGraph", reference_graph);
            apply("AstIndex", ast_index);
            // ContextCache's own invalidation (see its class comment):
            // any FileChanged event discards every cached intent, since
            // File/Keyword retrieval re-scan the whole project on every
            // call regardless of intent -- a per-file dependency list
            // would be false precision here.
            context_cache.InvalidateAll();
        });
    file_watcher.Start();
    AISTUDIO_LOG_INFO("Core.MCP", "FileWatcher: watching '" + project_root + "' for changes");

    const McpServer server(McpServerOptions{
        .symbol_index = &symbol_index,
        .context_retriever = &context_retriever,
        .context_cache = &context_cache,
        .context_response_budget_tokens = context_response_budget_tokens,
        .suppress_resent_content = suppress_resent_content,
        .sent_ledger = suppress_resent_content ? &sent_ledger : nullptr,
        .include_graph = &include_graph,
        .call_graph = &call_graph,
        .inheritance_graph = &inheritance_graph,
        .reference_graph = &reference_graph,
        .ast_index = &ast_index,
        .impact_analyzer = &impact_analyzer,
        .backend_registry = &registry,
        .enable_git_write_commands = enable_git_write_commands,
        .editor_state_store = &editor_state_store,
        .project_root = project_root,
        .firewall = &sandbox,
    });

    AISTUDIO_LOG_INFO("Core.MCP",
                       "context_retrieve response budget: " +
                           (context_response_budget_tokens > 0
                                 ? std::to_string(context_response_budget_tokens) + " token(s) (estimate)"
                                 : std::string("disabled (unbudgeted, set mcp.context_response_budget_tokens to "
                                                "enable)")));
    AISTUDIO_LOG_INFO("Core.MCP", std::string("context_retrieve sent ledger: ") +
                                       (suppress_resent_content
                                            ? "enabled (unchanged items sent as stubs)"
                                            : "disabled (set mcp.suppress_resent_content to enable)"));

    AISTUDIO_LOG_INFO("Core.MCP", "ready: symbol_index=" + std::to_string(symbol_index.Size()) +
                                       " symbol(s), call_graph=" + std::to_string(call_graph.Size()) +
                                       " edge(s), inheritance_graph=" + std::to_string(inheritance_graph.Size()) +
                                       " edge(s), reference_graph=" + std::to_string(reference_graph.Size()) +
                                       " edge(s), ast_index=" + std::to_string(ast_index.Size()) +
                                       " file(s), backend_registry=" + std::to_string(registry.All().size()) +
                                       " backend(s), serving tools over stdio");
    server.Run(std::cin, std::cout);
    file_watcher.Stop();
    EventBus::Instance().Unsubscribe("FileChanged", file_watcher_subscription_id);
    AISTUDIO_LOG_INFO("Core.MCP", "stdin closed, shutting down");
    return 0;
}

// LSP stdio mode (docs/ROADMAP.md "## LSP", CLAUDE.md's 2026-09-04 note
// that LSP needs its own new subsystem, "one size bigger" than
// McpServer). Deliberately minimal slice (AGENT.md #14): lifecycle +
// document sync + five read-only capabilities +
// textDocument/publishDiagnostics (index-derived only -- no real
// compiler/build execution, see Core/LSP/LspServer.hpp's own doc
// comment) -- see that header's full scope decision and
// docs/ROADMAP.md's Current Status entries for this and prior LSP tasks
// for what's deferred and why. Same "quiet bootstrap, stdout carries
// nothing but protocol messages" design as RunMcpMode() above, for the
// same reason (Content-Length framing is just as intolerant of stray
// std::cout writes as MCP's newline-delimited framing is) -- and the
// same reason this doesn't build a BackendRegistry/ContextRetriever/the
// other graph indexes RunMcpMode() does: this slice only ever needed
// SymbolIndex, plus now IncludeGraph for publishDiagnostics's
// unresolved-`#include` half.
int RunLspMode() {
    Logger::Instance().SetMinLevel(LogLevel::Info);
    // Separate log file from RunMcpMode()'s aistudio_mcp.log (both an
    // MCP client and an LSP client could plausibly run against the same
    // project at once) -- same OS-temp-directory placement and same
    // reasoning (see RunMcpMode()'s own comment on why: keeping it out
    // of anything GitBackend's git.commit "git add -A" could ever touch).
    Logger::Instance().SetLogFile(PathToUtf8(std::filesystem::temp_directory_path() / PerProcessLogFileName("aistudio_lsp")));
    Logger::Instance().SetConsoleEnabled(false);
    AISTUDIO_LOG_INFO("Core.LSP", "AI Development Studio Core starting in LSP stdio mode");

    Config config;
    if (const auto load_result = config.LoadFromFile("aistudio.config"); !load_result) {
        AISTUDIO_LOG_WARN("Core.LSP", "no config file found, using defaults: " + load_result.Err().message);
    }
    const auto project_root = config.GetOr("project.root", ".");
    const auto extra_ignore_patterns = LoadExtraIgnorePatterns(config);
    // Opt-in only (docs/ROADMAP.md's Rename entry, resolved by the user's
    // explicit decision to mirror RunMcpMode()'s enable_git_write_commands
    // exactly): textDocument/rename / textDocument/prepareRename stay
    // absent from `initialize`'s advertised capabilities, and are actively
    // rejected if called anyway, unless an operator explicitly sets this
    // in aistudio.config -- the connecting LSP client's own permission
    // model (or simply not applying the returned WorkspaceEdit) is the
    // intended safeguard, not a new Studio-side approval mechanism. See
    // LspServerOptions::enable_rename's own comment for why this stays
    // under the same "mcp." key family as enable_git_write_commands even
    // though it gates an LSP (not MCP) capability.
    const bool enable_lsp_rename = config.GetOr("mcp.enable_lsp_rename", "false") == "true";

    // Context Firewall (docs/MASTER_SPEC.md #21), same as RunMcpMode()'s
    // own `sandbox` -- applied to every textDocument/definition result.
    const Sandbox sandbox(project_root);

    SymbolIndex symbol_index;
    if (const auto index_result = symbol_index.Build(project_root, extra_ignore_patterns); !index_result) {
        AISTUDIO_LOG_WARN("Core.LSP", "SymbolIndex build failed: " + index_result.Err().message);
    }

    // IncludeGraph, additionally built here (unlike this function's
    // pre-Diagnostics scope, which only ever needed SymbolIndex) purely
    // to source the unresolved-`#include` half of
    // textDocument/publishDiagnostics -- see LspServer.hpp's own comment
    // on that capability. Kept fresh for the lifetime of this stdio
    // session by the FileWatcher wired up below (docs/ROADMAP.md's
    // "未解決includeのstale診断" follow-up task) -- this Build() call is
    // now only the initial population, mirroring the exact same
    // Build()-then-FileWatcher-keeps-it-fresh shape RunMcpMode() and the
    // default HTTP mode already use for their own indexes.
    IncludeGraph include_graph;
    if (const auto graph_result = include_graph.Build(project_root, extra_ignore_patterns); !graph_result) {
        AISTUDIO_LOG_WARN("Core.LSP", "IncludeGraph build failed: " + graph_result.Err().message);
    }

    // FileWatcher (docs/ROADMAP.md's "未解決includeのstale診断" follow-up
    // task, CLAUDE.md's `feature/LspDiagnostics`/`feature/LspModeFileWatcher`
    // entries): keeps SymbolIndex/IncludeGraph fresh for as long as this
    // stdio session lives, instead of staying frozen at whatever they
    // looked like when this process started -- exactly the same problem
    // RunMcpMode()'s own FileWatcher block above already solved for its
    // six indexes, mirrored here for the two indexes this mode actually
    // builds (no CallGraph/InheritanceGraph/ReferenceGraph/AstIndex/
    // ContextCache exist in this mode -- see this function's own doc
    // comment above for why). Fixes the documented staleness limitation
    // in LspServer.hpp's textDocument/publishDiagnostics comment: without
    // this, editing an open document to remove the very `#include` line
    // that triggered a warning never cleared it, because include_graph
    // itself stayed frozen at process-startup content.
    FileWatcher file_watcher(project_root, FileScanner(FileScanner::MakeOptions(extra_ignore_patterns)));
    const auto file_watcher_subscription_id = EventBus::Instance().Subscribe(
        "FileChanged", [&symbol_index, &include_graph, &project_root](const std::any& payload) {
            const auto* change = std::any_cast<FileChangeEvent>(&payload);
            if (change == nullptr) {
                return;
            }
            // Same generic apply() shape as RunMcpMode()'s own subscriber,
            // narrowed to the two indexes this mode builds.
            const auto apply = [&](const char* index_name, auto& index) {
                const auto result = change->kind == FileChangeKind::Deleted
                                         ? index.RemoveFile(change->path)
                                         : index.UpdateFile(project_root, change->path);
                if (!result) {
                    AISTUDIO_LOG_WARN("Core.FileWatcher", std::string(index_name) +
                                                                " incremental update failed for '" + change->path +
                                                                "': " + result.Err().message);
                }
            };
            apply("SymbolIndex", symbol_index);
            apply("IncludeGraph", include_graph);
        });
    file_watcher.Start();
    AISTUDIO_LOG_INFO("Core.LSP", "FileWatcher: watching '" + project_root + "' for changes");

    // LspServerOptions::project_root is used to convert a resolved
    // Symbol::file_path back into a `file://` URI (see
    // Core/LSP/LspServer.cpp's ProjectRelativePathToUri) -- that
    // conversion needs an absolute path regardless of whether
    // `project.root` in aistudio.config was given as "." or something
    // relative; std::filesystem::absolute() resolves it against this
    // process's current working directory (the project root itself, for
    // a CLI-launched process -- same assumption RunMcpMode() already
    // makes for its own log-file placement).
    std::error_code absolute_ec;
    auto absolute_project_root = std::filesystem::absolute(Utf8ToPath(project_root), absolute_ec);
    if (absolute_ec) {
        AISTUDIO_LOG_WARN("Core.LSP", "failed to resolve project_root to an absolute path: " + absolute_ec.message());
        absolute_project_root = Utf8ToPath(project_root);
    }

    const LspServer server(LspServerOptions{
        .symbol_index = &symbol_index,
        .project_root = PathToUtf8(absolute_project_root.lexically_normal()),
        .firewall = &sandbox,
        .include_graph = &include_graph,
        .enable_rename = enable_lsp_rename,
    });

#if defined(_WIN32)
    // Windows' CRT opens stdin/stdout in text mode by default, which
    // rewrites every '\n' a stream writes to '\r\n' -- silently corrupting
    // WriteMessage's own literal "\r\n\r\n" header terminator into
    // "\r\r\n\r\r\n" (each embedded '\n' gets a second '\r' inserted in
    // front of it) before it ever reaches the OS pipe/file, no matter
    // whether the destination is a console, a redirected file, or a real
    // LSP client's pipe. This went unnoticed by every existing LspServer
    // unit test (test_lsp_server.cpp) because they all drive
    // LspServer::Run() against in-memory std::istringstream/ostringstream
    // -- std::cin/std::cout's own text-mode translation is a std::iostream
    // detail bound in main()/RunLspMode(), not something LspServer::Run()
    // itself ever touches, so no test exercises it. Only discovered by
    // actually piping raw Content-Length-framed bytes through a real,
    // separately-launched aistudio_core_cli.exe --lsp process (this
    // task's own real-machine verification of documentSymbol/workspace-
    // symbol) and hex-dumping its literal stdout bytes. _setmode to
    // _O_BINARY disables that CRT-level translation so the bytes
    // LspServer::Run() writes reach the client exactly as framed.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    AISTUDIO_LOG_INFO("Core.LSP", "ready: symbol_index=" + std::to_string(symbol_index.Size()) +
                                       " symbol(s), include_graph=" + std::to_string(include_graph.Size()) +
                                       " edge(s), serving over stdio (Content-Length framing)");
    server.Run(std::cin, std::cout);
    file_watcher.Stop();
    EventBus::Instance().Unsubscribe("FileChanged", file_watcher_subscription_id);
    AISTUDIO_LOG_INFO("Core.LSP", "stdin closed or exit received, shutting down");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--mcp") {
            return RunMcpMode();
        }
        if (std::string_view(argv[i]) == "--lsp") {
            return RunLspMode();
        }
    }

    Logger::Instance().SetMinLevel(LogLevel::Trace);
    AISTUDIO_LOG_INFO("Core.Bootstrap", "AI Development Studio Core starting");

    Config config;
    const auto load_result = config.LoadFromFile("aistudio.config");
    if (!load_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "no config file found, using defaults: " + load_result.Err().message);
    }
    const auto studio_name = config.GetOr("studio.name", "AI Development Studio");

    // Secret Protection (docs/MASTER_SPEC.md #73): a placeholder value
    // standing in for a real LLM provider API key (Phase 4, not
    // implemented yet) — demonstrates that ToString() never leaks it and
    // that Get()/SaveToFile() (used just above/below) never see it.
    config.SetSecret("llm.api_key", "sk-placeholder-never-a-real-key");
    if (const auto api_key = config.GetSecret("llm.api_key")) {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "Config: llm.api_key = " + api_key->ToString() + " (Get() sees it? " +
                                                 std::to_string(config.Get("llm.api_key").has_value()) + ")");
    }

    EventBus::Instance().Subscribe("BackendConnected", [](const std::any& payload) {
        if (const auto* id = std::any_cast<std::string>(&payload)) {
            AISTUDIO_LOG_INFO("Core.Event", "BackendConnected: " + *id);
        }
    });

    BackendRegistry registry;
    for (auto& discovered : BackendFactoryRegistry::Instance().CreateAll()) {
        const auto id = discovered->Id();
        if (const auto register_result = registry.Register(discovered); !register_result) {
            AISTUDIO_LOG_ERROR("Core.Bootstrap", "failed to register backend: " + register_result.Err().message);
            return 1;
        }
        EventBus::Instance().Publish("BackendConnected", id);
    }
    AISTUDIO_LOG_INFO("Core.Bootstrap", "BackendFactoryRegistry discovered " +
                                             std::to_string(BackendFactoryRegistry::Instance().FactoryCount()) +
                                             " backend(s)");

    // plugin_registry is declared before the LoadConfiguredPluginBackends()
    // loop below (not after, as it once was) so each loaded Plugin's
    // manifest can be registered into it inline -- previously nothing
    // ever registered a manifest for a REAL loaded Plugin (only the
    // hardcoded core.null one below), so UndeclaredCapabilities() always
    // false-positived on every capability every real Plugin advertised.
    PluginRegistry plugin_registry;

    // Plugin DLLs declared via `plugin.ids`/`plugin.<id>.dll` in
    // aistudio.config (docs/ROADMAP.md Phase 11 "Plugin SDK" -- see
    // LoadConfiguredPluginBackends()'s own doc comment for why this is
    // opt-in-by-config rather than e.g. scanning a directory). Empty by
    // default: nothing here changes behavior unless the user's own
    // config lists a plugin. `plugin_loader` must outlive every Backend
    // this returns, so it's declared here rather than inside the loop.
    PluginLoader plugin_loader;
    for (auto& plugin_backend : LoadConfiguredPluginBackends(config, plugin_loader)) {
        const auto id = plugin_backend->Id();
        if (const auto register_result = registry.Register(plugin_backend); !register_result) {
            AISTUDIO_LOG_WARN("Core.Bootstrap",
                               "failed to register plugin backend '" + id + "': " + register_result.Err().message);
            continue;
        }
        EventBus::Instance().Publish("BackendConnected", id);

        // required_permissions is always empty here -- ManifestFromBackend()'s
        // own doc comment explains why (AbiV1.h has no channel for a
        // Plugin to declare these yet).
        if (const auto manifest_result = plugin_registry.RegisterManifest(ManifestFromBackend(*plugin_backend));
            !manifest_result) {
            AISTUDIO_LOG_WARN("Core.Bootstrap",
                               "failed to register plugin manifest '" + id + "': " + manifest_result.Err().message);
        }
    }

    PluginManifest null_backend_manifest;
    null_backend_manifest.id = "core.null";
    null_backend_manifest.name = "Null Backend";
    null_backend_manifest.version = "0.1.0";
    null_backend_manifest.capabilities = {"diagnostics.ping@1.0.0", "diagnostics.delete"};
    if (const auto manifest_result = plugin_registry.RegisterManifest(null_backend_manifest); !manifest_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "failed to register plugin manifest: " + manifest_result.Err().message);
    }
    if (const auto undeclared = plugin_registry.UndeclaredCapabilities(registry); !undeclared.empty()) {
        AISTUDIO_LOG_WARN("Core.Bootstrap",
                           "backend(s) advertise " + std::to_string(undeclared.size()) + " undeclared capability(ies)");
    } else {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "PluginRegistry: all backend capabilities are declared by a manifest");
    }

    if (const auto configure_result = registry.ConfigureAll(config); !configure_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "backend configuration issues: " + configure_result.Err().message);
    }

    if (const auto start_result = registry.StartAll(); !start_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "backend startup issues: " + start_result.Err().message);
    }

    for (const auto& [id, health] : registry.CheckHealth()) {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "backend health: " + id + " = " + ToString(health));
    }

    // Capability System versioning/negotiation (docs/ROADMAP.md):
    // core.null advertises "diagnostics.ping@1.0.0" — a caller requiring
    // 1.0.0 or lower-within-major is satisfied, a caller requiring an
    // incompatible major version is not, even though both ask by the
    // same capability name.
    const auto compatible_ping =
        registry.FindByCapability("diagnostics.ping", CapabilityVersion{.major = 1, .minor = 0, .patch = 0});
    const auto incompatible_ping =
        registry.FindByCapability("diagnostics.ping", CapabilityVersion{.major = 2, .minor = 0, .patch = 0});
    AISTUDIO_LOG_INFO("Core.Bootstrap", "Capability negotiation: diagnostics.ping@1.0.0 required -> " +
                                             std::to_string(compatible_ping.size()) +
                                             " backend(s), @2.0.0 required -> " +
                                             std::to_string(incompatible_ping.size()) + " backend(s)");

    const auto ping_backends = registry.FindByCapability("diagnostics.ping");
    if (ping_backends.empty()) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "no backend advertises diagnostics.ping; skipping ping demo");
    } else {
        Command ping;
        ping.request_id = RequestIdGenerator::Next();
        ping.backend_id = ping_backends.front()->Id();
        ping.name = "diagnostics.ping";
        const auto ping_result = registry.Dispatch(ping);
        if (ping_result) {
            AISTUDIO_LOG_INFO("Core.Bootstrap", "diagnostics.ping [" + ping.request_id + "] -> " +
                                                     std::any_cast<std::string>(ping_result.Value()));
        } else {
            AISTUDIO_LOG_ERROR("Core.Bootstrap", "diagnostics.ping failed: " + ping_result.Err().message);
        }
    }

    std::cout << studio_name << " Core bootstrap OK. Registered backends:\n";
    for (const auto& b : registry.All()) {
        std::cout << "  - " << b->Name() << " (" << b->Id() << ") capabilities: ";
        for (const auto& cap : b->Capabilities()) {
            std::cout << cap << " ";
        }
        std::cout << "\n";
    }

    Database db;
    if (const auto open_result = db.Open("aistudio.db"); !open_result) {
        AISTUDIO_LOG_ERROR("Core.Bootstrap", "failed to open database: " + open_result.Err().message);
        return 1;
    }

    // Declared at function scope (not just inside the file-scan block
    // below that populates it) so it's also reachable at ApiServer
    // construction further down, wiring /api/context/usage to real data
    // (docs/ROADMAP.md "Context UI").
    ContextAuditRepository audit_repository(db);
    audit_repository.EnsureSchema();

    // Same reasoning as audit_repository above: declared here (not just
    // inside the file-scan block that first populates it) so it's also
    // reachable at ApiServer construction further down, wiring
    // /api/context/snapshots* to real data (docs/ROADMAP.md Phase 2
    // "Context Restore").
    ContextSnapshotRepository snapshot_repository(db);
    if (const auto ensure_result = snapshot_repository.EnsureSchema(); !ensure_result) {
        AISTUDIO_LOG_ERROR("Core.Bootstrap", "failed to prepare context_snapshots table: " + ensure_result.Err().message);
    }

    Migrator migrator(db);
    const std::vector<Migration> migrations = {
        {1, "create_tasks_table",
         "CREATE TABLE IF NOT EXISTS tasks ("
         "  id TEXT PRIMARY KEY,"
         "  name TEXT NOT NULL,"
         "  state INTEGER NOT NULL,"
         "  progress REAL NOT NULL,"
         "  retry_count INTEGER NOT NULL,"
         "  max_retries INTEGER NOT NULL,"
         "  depends_on TEXT NOT NULL DEFAULT ''"
         ");"},
    };
    if (const auto migrate_result = migrator.Apply(migrations); !migrate_result) {
        AISTUDIO_LOG_ERROR("Core.Bootstrap", "migration failed: " + migrate_result.Err().message);
        return 1;
    }

    TaskRepository task_repository(db);
    Task bootstrap_task;
    bootstrap_task.id = "core.bootstrap";
    bootstrap_task.name = "Core bootstrap self-check";
    bootstrap_task.state = TaskState::Completed;
    bootstrap_task.progress = 1.0;
    if (const auto save_result = task_repository.Save(bootstrap_task); !save_result) {
        AISTUDIO_LOG_ERROR("Core.Bootstrap", "failed to persist bootstrap task: " + save_result.Err().message);
        return 1;
    }

    const auto all_tasks_result = task_repository.FindAll();
    if (all_tasks_result) {
        AISTUDIO_LOG_INFO("Core.Bootstrap",
                           "aistudio.db: tasks table has " + std::to_string(all_tasks_result.Value().size()) + " row(s)");
    }

    const auto project_root = config.GetOr("project.root", ".");
    const auto extra_ignore_patterns = LoadExtraIgnorePatterns(config);

    // Security "Sandbox" (docs/MASTER_SPEC.md #73): demonstrate that a
    // path inside project_root is allowed, while a path escaping it (or
    // matching a deny pattern like ".git") is rejected.
    const Sandbox sandbox(project_root);
    AISTUDIO_LOG_INFO("Core.Bootstrap", "Sandbox: 'src/main.cpp' allowed = " +
                                             std::to_string(sandbox.IsAllowed("src/main.cpp")) +
                                             ", '../outside.txt' allowed = " +
                                             std::to_string(sandbox.IsAllowed("../outside.txt")) +
                                             ", '.git/config' allowed = " +
                                             std::to_string(sandbox.IsAllowed(".git/config")));

    FileScanner scanner(FileScanner::MakeOptions(extra_ignore_patterns));
    const auto scan_result = scanner.Scan(project_root);
    if (scan_result) {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "FileScanner: found " + std::to_string(scan_result.Value().size()) +
                                                 " file(s) under '" + project_root + "'");

        FileCache file_cache;
        if (!scan_result.Value().empty()) {
            const auto& first = scan_result.Value().front();
            // Demonstrates the Put -> hit -> (simulated) staleness -> miss
            // cycle FileCache is meant for; not a real content cache yet
            // since we don't re-read the file content here.
            file_cache.Put(first, "(not loaded)");
            const auto cached = file_cache.Get(first);
            AISTUDIO_LOG_INFO("Core.Bootstrap", "FileCache: " + std::string(cached ? "hit" : "miss") + " for '" +
                                                     first.path + "' (hash=" + first.content_hash + ")");
        }

        // Context Firewall (docs/MASTER_SPEC.md #21): FileContextSource
        // itself is a pure metadata->ContextItem conversion with no
        // knowledge of Sandbox, so filtering is this call site's own
        // responsibility -- the same way ContextRetriever filters before
        // ever constructing a ContextItem for a denied path. This
        // `candidates` list ends up persisted into a ContextSnapshot
        // below, so an unfiltered entry here (e.g. under `.git` or a
        // secret-like filename) wouldn't just be a demo-log artifact, it
        // would be written to disk.
        std::vector<ContextItem> candidates;
        candidates.reserve(scan_result.Value().size());
        for (const auto& metadata : scan_result.Value()) {
            if (sandbox.IsAllowed(metadata.path)) {
                candidates.push_back(MakeFileContextItem(metadata));
            }
        }

        const auto audit_subscription_id =
            EventBus::Instance().Subscribe("ContextAudit", [&audit_repository](const std::any& payload) {
                if (const auto* entry = std::any_cast<ContextAuditEntry>(&payload)) {
                    audit_repository.Save(*entry);
                }
            });

        const ContextSelector selector;
        const auto selection = selector.Select(std::move(candidates), ContextBudget(2000));

        EventBus::Instance().Unsubscribe("ContextAudit", audit_subscription_id);

        AISTUDIO_LOG_INFO("Core.Bootstrap", "ContextSelector: included " + std::to_string(selection.included.size()) +
                                                 " / excluded " + std::to_string(selection.excluded.size()) +
                                                 " file(s), used_tokens=" + std::to_string(selection.used_tokens) +
                                                 " (budget=2000)");

        if (const auto audit_count_result = audit_repository.FindAll(); audit_count_result) {
            AISTUDIO_LOG_INFO("Core.Bootstrap",
                               "ContextAudit: recorded " + std::to_string(audit_count_result.Value().size()) + " entries");
        }

        const auto snapshot = MakeContextSnapshot("core.bootstrap.scan", "Core bootstrap file scan", selection, 2000);
        if (const auto save_result = snapshot_repository.Save(snapshot); !save_result) {
            AISTUDIO_LOG_ERROR("Core.Bootstrap", "failed to save context snapshot: " + save_result.Err().message);
        } else {
            AISTUDIO_LOG_INFO("Core.Bootstrap", "ContextSnapshot saved: '" + snapshot.id + "' (" +
                                                     std::to_string(snapshot.included_item_ids.size()) + " item ids)");
        }
    } else {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "FileScanner failed for '" + project_root + "': " + scan_result.Err().message);
    }

    SymbolIndex symbol_index;
    if (const auto index_result = symbol_index.Build(project_root, extra_ignore_patterns); !index_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "SymbolIndex build failed: " + index_result.Err().message);
    } else {
        AISTUDIO_LOG_INFO("Core.Bootstrap",
                           "SymbolIndex: indexed " + std::to_string(symbol_index.Size()) + " symbol(s) (tree-sitter-cpp — see SymbolExtractor caveats)");
        const auto cache_stats = symbol_index.Stats();
        AISTUDIO_LOG_INFO("Core.Bootstrap", "SymbolIndex cache: " + std::to_string(cache_stats.hits) + " hit(s), " +
                                                 std::to_string(cache_stats.misses) + " miss(es), " +
                                                 std::to_string(cache_stats.size) + " entrie(s)");
    }

    IncludeGraph include_graph;
    if (const auto graph_result = include_graph.Build(project_root, extra_ignore_patterns); !graph_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "IncludeGraph build failed: " + graph_result.Err().message);
    } else {
        const auto include_edges = include_graph.AllEdges();
        const auto resolved_count =
            std::count_if(include_edges.begin(), include_edges.end(),
                           [](const IncludeEdge& edge) { return !edge.resolved_path.empty(); });
        AISTUDIO_LOG_INFO("Core.Bootstrap", "IncludeGraph: indexed " + std::to_string(include_graph.Size()) +
                                                 " include edge(s), " + std::to_string(resolved_count) +
                                                 " resolved to a project file");
        const auto cache_stats = include_graph.Stats();
        AISTUDIO_LOG_INFO("Core.Bootstrap", "IncludeGraph cache: " + std::to_string(cache_stats.hits) + " hit(s), " +
                                                 std::to_string(cache_stats.misses) + " miss(es), " +
                                                 std::to_string(cache_stats.size) + " entrie(s)");

        // Context Retrieval "Dependency retrieval" (docs/ROADMAP.md Phase 2),
        // now backed by IncludeGraph: demonstrate turning src/main.cpp's own
        // direct includes into compact Reference-level ContextItems.
        const auto dependency_items =
            MakeDependencyContextItems(include_graph, "src/main.cpp", DependencyDirection::Includes);
        AISTUDIO_LOG_INFO("Core.Bootstrap", "DependencyContextSource: 'src/main.cpp' has " +
                                                 std::to_string(dependency_items.size()) + " resolved dependencies");
    }

    CallGraph call_graph;
    if (const auto call_graph_result = call_graph.Build(project_root, extra_ignore_patterns); !call_graph_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "CallGraph build failed: " + call_graph_result.Err().message);
    } else {
        AISTUDIO_LOG_INFO("Core.Bootstrap",
                           "CallGraph: indexed " + std::to_string(call_graph.Size()) + " call edge(s)");
        const auto cache_stats = call_graph.Stats();
        AISTUDIO_LOG_INFO("Core.Bootstrap", "CallGraph cache: " + std::to_string(cache_stats.hits) + " hit(s), " +
                                                 std::to_string(cache_stats.misses) + " miss(es), " +
                                                 std::to_string(cache_stats.size) + " entrie(s)");

        // Phase 3 "Impact Analysis" (docs/ROADMAP.md): demonstrate asking
        // "what would be affected by changing Core/Index/SymbolIndex.hpp?"
        // now that IncludeGraph + CallGraph + SymbolIndex are all built.
        const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);
        const auto impact = impact_analyzer.Analyze("include/Core/Index/SymbolIndex.hpp");
        AISTUDIO_LOG_INFO("Core.Bootstrap", "ImpactAnalyzer: changing '" + impact.target_file + "' affects " +
                                                 std::to_string(impact.affected_files.size()) +
                                                 " file(s) transitively and " +
                                                 std::to_string(impact.affected_symbols.size()) +
                                                 " symbol(s) defined there");
    }

    InheritanceGraph inheritance_graph;
    if (const auto inheritance_result = inheritance_graph.Build(project_root, extra_ignore_patterns); !inheritance_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "InheritanceGraph build failed: " + inheritance_result.Err().message);
    } else {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "InheritanceGraph: indexed " + std::to_string(inheritance_graph.Size()) +
                                                 " inheritance edge(s)");
        const auto cache_stats = inheritance_graph.Stats();
        AISTUDIO_LOG_INFO("Core.Bootstrap",
                           "InheritanceGraph cache: " + std::to_string(cache_stats.hits) + " hit(s), " +
                               std::to_string(cache_stats.misses) + " miss(es), " +
                               std::to_string(cache_stats.size) + " entrie(s)");
    }

    ReferenceGraph reference_graph;
    if (const auto reference_result = reference_graph.Build(project_root, extra_ignore_patterns); !reference_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "ReferenceGraph build failed: " + reference_result.Err().message);
    } else {
        AISTUDIO_LOG_INFO("Core.Bootstrap",
                           "ReferenceGraph: indexed " + std::to_string(reference_graph.Size()) + " type reference(s)");
        const auto cache_stats = reference_graph.Stats();
        AISTUDIO_LOG_INFO("Core.Bootstrap", "ReferenceGraph cache: " + std::to_string(cache_stats.hits) + " hit(s), " +
                                                 std::to_string(cache_stats.misses) + " miss(es), " +
                                                 std::to_string(cache_stats.size) + " entrie(s)");
    }

    AstIndex ast_index;
    if (const auto ast_result = ast_index.Build(project_root, extra_ignore_patterns); !ast_result) {
        AISTUDIO_LOG_WARN("Core.Bootstrap", "AstIndex build failed: " + ast_result.Err().message);
    } else {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "AstIndex: indexed " + std::to_string(ast_index.Size()) + " file(s)");
        const auto cache_stats = ast_index.Stats();
        AISTUDIO_LOG_INFO("Core.Bootstrap", "AstIndex cache: " + std::to_string(cache_stats.hits) + " hit(s), " +
                                                 std::to_string(cache_stats.misses) + " miss(es), " +
                                                 std::to_string(cache_stats.size) + " entrie(s)");

        // Context Compression "AST representation" (docs/ROADMAP.md
        // Phase 2), now backed by AstIndex: demonstrate rendering
        // src/main.cpp's own structural outline as a compact
        // CompressionLevel::Ast ContextItem.
        if (const auto tree = ast_index.Get("src/main.cpp"); tree.has_value()) {
            const auto ast_item = MakeAstContextItem("src/main.cpp", *tree);
            AISTUDIO_LOG_INFO("Core.Bootstrap", "AstContextSource: 'src/main.cpp' outline is ~" +
                                                     std::to_string(ast_item.estimated_tokens) + " token(s)");
        }
    }

    // Phase 3 "Semantic Search" > "Symbol Search"/"Keyword Search"
    // (docs/ROADMAP.md): demonstrate ranked lookup on top of the
    // SymbolIndex built above, and a plain text search across the
    // project.
    const SymbolSearch symbol_search;
    const auto symbol_matches = symbol_search.Search(symbol_index, "Index");
    AISTUDIO_LOG_INFO("Core.Bootstrap",
                       "SymbolSearch: 'Index' matched " + std::to_string(symbol_matches.size()) + " symbol(s)");

    const KeywordSearch keyword_search;
    if (const auto keyword_result = keyword_search.Search(project_root, "TODO"); keyword_result) {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "KeywordSearch: 'TODO' matched " +
                                                 std::to_string(keyword_result.Value().size()) + " line(s)");
    }

    // active_document / Active File Bias (docs/ROADMAP.md "## IDE"):
    // reads the same %TEMP%/aistudio_editor_state_<hash>.json a connected
    // VS Code extension writes (see WorkspaceHash.hpp for how <hash> is
    // derived from project_root) -- see EditorStateStore.hpp for why this HTTP
    // bootstrap and RunMcpMode() above both build their own instance
    // rather than sharing one (they're separate processes/mode branches
    // in main(), never running at the same time). No active_document MCP
    // tool exists in this mode (that's MCP-only), but ContextRetriever
    // below still consults it for Active File Bias -- if a VS Code window
    // with this same project_root open happens to be running while this
    // HTTP mode is also running, its context_retrieve results are biased
    // toward whatever file that IDE currently has focused; if not (the
    // common case for this demo bootstrap), the file is simply absent and
    // this has zero effect, exactly as before Active File Bias existed.
    const EditorStateStore editor_state_store(EditorStateStore::Options{
        .project_root = project_root,
        .firewall = &sandbox,
    });

    // Context Retrieval "File retrieval"/"Symbol retrieval"/"Dependency
    // retrieval" made intent-driven (docs/ROADMAP.md Phase 2): given a
    // free-text intent instead of a specific file/symbol, select what's
    // actually relevant rather than turning something already-chosen
    // into a ContextItem. Reuses the same `sandbox` instance built above
    // as its Context Firewall (docs/MASTER_SPEC.md #21) — a match whose
    // file the Sandbox would deny (e.g. under .git, or secret-like) is
    // never turned into a ContextItem here either.
    const ContextRetriever context_retriever(ContextRetriever::Options{
        .symbol_index = &symbol_index,
        .include_graph = &include_graph,
        .project_root = project_root,
        .firewall = &sandbox,
        // Any registered Backend (native or Plugin, including one loaded
        // via LoadConfiguredPluginBackends above) gets a chance to
        // contribute context too (docs/MASTER_SPEC.md #71). RunMcpMode()
        // above deliberately doesn't do this -- it never builds a
        // BackendRegistry at all, an existing, separate scope boundary
        // this doesn't change.
        .backend_registry = &registry,
        .editor_state_store = &editor_state_store,
    });

    // Active File Bias real-machine verification (docs/DEVELOPMENT_PROTOCOL.md
    // #6 Build/Test/Error-Check/Review): rather than only trusting
    // test_context_retriever.cpp's synthetic-fixture coverage, prove the
    // bias actually changes ranking against THIS repository's own files.
    // Writes a throwaway EditorStateStore state file (a dedicated temp
    // path, never EditorStateStore::DefaultStateFilePath(), so this can
    // never collide with a real IDE extension's own state file on the
    // same machine) reporting "src/main.cpp" -- the very file
    // DependencyContextSource was just demonstrated against above -- as
    // the active document, then compares SymbolSearch's own top
    // "RunMcpMode" match's priority (a function defined in main.cpp
    // itself, so this is guaranteed to be a real match there rather than
    // hoping some other symbol matching "Index" happens to also live in
    // main.cpp) with and without that bias wired in.
    {
        const std::string demo_state_file_path =
            PathToUtf8(std::filesystem::temp_directory_path() / "aistudio_editor_state_bootstrap_demo.json");
        const std::string demo_active_document = PathToUtf8(Utf8ToPath(project_root) / "src" / "main.cpp");
        {
            std::ofstream demo_state_out(Utf8ToPath(demo_state_file_path), std::ios::binary);
            demo_state_out << nlohmann::json{
                {"version", 1},
                {"source", "bootstrap-demo"},
                {"active_document_path", demo_active_document},
                {"selection", nullptr},
                {"updated_at", ""},
            }.dump();
        }
        const EditorStateStore demo_editor_state_store(EditorStateStore::Options{
            .state_file_path = demo_state_file_path,
            .project_root = project_root,
            .firewall = &sandbox,
        });
        const ContextRetriever unbiased_retriever(ContextRetriever::Options{
            .symbol_index = &symbol_index, .include_graph = &include_graph, .project_root = project_root});
        const ContextRetriever biased_retriever(ContextRetriever::Options{.symbol_index = &symbol_index,
                                                                            .include_graph = &include_graph,
                                                                            .project_root = project_root,
                                                                            .editor_state_store =
                                                                                &demo_editor_state_store});
        const auto find_main_cpp_priority = [](const std::vector<ContextItem>& items) -> std::int64_t {
            for (const auto& item : items) {
                if (item.id.rfind("src/main.cpp:", 0) == 0) {
                    return item.priority;
                }
            }
            return -1;
        };
        const auto unbiased_priority = find_main_cpp_priority(unbiased_retriever.Retrieve("RunMcpMode"));
        const auto biased_priority = find_main_cpp_priority(biased_retriever.Retrieve("RunMcpMode"));
        if (unbiased_priority >= 0 && biased_priority >= 0) {
            AISTUDIO_LOG_INFO("Core.Bootstrap", "ActiveFileBias: 'src/main.cpp' symbol match priority " +
                                                     std::to_string(unbiased_priority) + " -> " +
                                                     std::to_string(biased_priority) +
                                                     " when reported as the active document");
        } else {
            AISTUDIO_LOG_WARN(
                "Core.Bootstrap",
                "ActiveFileBias demo: no 'RunMcpMode' symbol match in 'src/main.cpp' to compare (project_root='" +
                    project_root + "') -- skipping verification log");
        }
        std::error_code remove_ec;
        std::filesystem::remove(Utf8ToPath(demo_state_file_path), remove_ec);
    }

    // Context Cache (docs/ROADMAP.md Phase 2 "Context Cache"): memoizes
    // ContextRetriever::Retrieve() results by intent. Demonstrated here
    // (rather than in RunMcpMode(), see McpServerOptions::context_cache's
    // own doc comment for why) specifically because this HTTP bootstrap
    // path already runs a real FileWatcher below, which is what makes
    // caching Retrieve() results safe at all -- see ContextCache's class
    // comment for why File/Keyword retrieval re-scanning the whole
    // project on every call means "the whole project changed" is the
    // only honestly correct invalidation granularity, and InvalidateAll()
    // (wired to the same "FileChanged" subscription that already drives
    // the six indices below) is how that's applied here.
    ContextCache context_cache;
    const auto retrieved = context_cache.GetOrRetrieve(context_retriever, "symbol index");
    AISTUDIO_LOG_INFO("Core.Bootstrap",
                       "ContextRetriever: intent 'symbol index' -> " + std::to_string(retrieved.size()) + " item(s)");

    // LLM Integration (docs/ROADMAP.md Phase 4): EchoLLMProvider is a
    // no-network stand-in proving the request/response/token-counting
    // plumbing end-to-end before a real provider (Anthropic/OpenAI/...)
    // is wired in — that needs an explicit decision on API key handling
    // first. Demonstrates the full "Context Engine -> LLM" flow AGENT.md
    // #8 describes: the ContextRetriever result retrieved just above
    // becomes the user message content here.
    EchoLLMProvider llm_provider;
    LLMRequest llm_request;
    llm_request.model = "aistudio-echo";
    llm_request.messages.push_back(ChatMessage{
        ChatRole::User,
        "Context items retrieved: " + std::to_string(retrieved.size()),
    });
    if (const auto llm_result = llm_provider.Complete(llm_request); llm_result) {
        AISTUDIO_LOG_INFO("Core.Bootstrap", "LLM (" + llm_provider.Name() + "): '" + llm_result.Value().content +
                                                 "' (" + std::to_string(llm_result.Value().usage.input_tokens) +
                                                 " input token(s), " +
                                                 std::to_string(llm_result.Value().usage.output_tokens) +
                                                 " output token(s))");
    }

    AISTUDIO_LOG_INFO("Core.Bootstrap", "AI Development Studio Core bootstrap complete");

    // Keeps SymbolIndex/IncludeGraph/CallGraph/InheritanceGraph/
    // ReferenceGraph/AstIndex fresh for as long as the API server runs,
    // without needing a restart or a manual re-Build() — docs/ROADMAP.md
    // "File Intelligence" > "Incremental Reload" / "File Watcher", now
    // that all six have their own UpdateFile()/RemoveFile() for it to
    // drive.
    FileWatcher file_watcher(project_root, FileScanner(FileScanner::MakeOptions(extra_ignore_patterns)));
    const auto file_watcher_subscription_id = EventBus::Instance().Subscribe(
        "FileChanged",
        [&symbol_index, &include_graph, &call_graph, &inheritance_graph, &reference_graph, &ast_index,
         &context_cache, &project_root](const std::any& payload) {
            const auto* change = std::any_cast<FileChangeEvent>(&payload);
            if (change == nullptr) {
                return;
            }
            // Every index below shares the same UpdateFile(root, path) /
            // RemoveFile(path) -> Result<void> shape, so one generic
            // lambda replaces what would otherwise be five near-identical
            // if/else + log blocks.
            const auto apply = [&](const char* index_name, auto& index) {
                const auto result = change->kind == FileChangeKind::Deleted
                                         ? index.RemoveFile(change->path)
                                         : index.UpdateFile(project_root, change->path);
                if (!result) {
                    AISTUDIO_LOG_WARN("Core.FileWatcher", std::string(index_name) +
                                                                " incremental update failed for '" + change->path +
                                                                "': " + result.Err().message);
                }
            };
            apply("SymbolIndex", symbol_index);
            apply("IncludeGraph", include_graph);
            apply("CallGraph", call_graph);
            apply("InheritanceGraph", inheritance_graph);
            apply("ReferenceGraph", reference_graph);
            apply("AstIndex", ast_index);
            // ContextCache's own invalidation (see its class comment):
            // any FileChanged event discards every cached intent, since
            // File/Keyword retrieval re-scan the whole project on every
            // call regardless of intent -- a per-file dependency list
            // would be false precision here.
            context_cache.InvalidateAll();
        });
    file_watcher.Start();
    AISTUDIO_LOG_INFO("Core.Bootstrap", "FileWatcher: watching '" + project_root + "' for changes");

    const auto api_host = config.GetOr("api.host", "127.0.0.1");
    const auto api_port = std::stoi(config.GetOr("api.port", "8787"));

    ApiServer api_server(registry, task_repository, studio_name,
                          ApiServerIndexes{
                              .plugin_registry = &plugin_registry,
                              .symbol_index = &symbol_index,
                              .include_graph = &include_graph,
                              .call_graph = &call_graph,
                              .inheritance_graph = &inheritance_graph,
                              .reference_graph = &reference_graph,
                              .ast_index = &ast_index,
                              .project_root = project_root,
                              .llm_provider = &llm_provider,
                              .context_audit_repository = &audit_repository,
                              .context_snapshot_repository = &snapshot_repository,
                              .context_firewall = &sandbox,
                          });

    // "Permission definition" (docs/ROADMAP.md Phase 11 "Plugin SDK") --
    // bridges every registered manifest's required_permissions into the
    // ApiServer's own PermissionPolicy, now that plugin_registry's
    // manifests are settled. Currently a no-op in practice (no manifest
    // registered above ever sets required_permissions -- see
    // ManifestFromBackend()'s doc comment), but wires the path so it
    // takes effect the moment something does.
    ApplyRequiredPermissions(plugin_registry, api_server.Policy());

    AISTUDIO_LOG_INFO("Core.API", "starting API server on http://" + api_host + ":" + std::to_string(api_port));
    std::cout << "API server listening on http://" << api_host << ":" << api_port << " (Ctrl+C to stop)\n";
    if (const auto listen_result = api_server.Listen(api_host, api_port); !listen_result) {
        file_watcher.Stop();
        EventBus::Instance().Unsubscribe("FileChanged", file_watcher_subscription_id);
        AISTUDIO_LOG_ERROR("Core.API", "failed to start API server: " + listen_result.Err().message);
        return 1;
    }
    file_watcher.Stop();
    EventBus::Instance().Unsubscribe("FileChanged", file_watcher_subscription_id);
    AISTUDIO_LOG_INFO("Core.API", "API server stopped");
    return 0;
}
