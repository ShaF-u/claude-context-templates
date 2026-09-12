#pragma once

#include "Core/Analysis/ImpactAnalyzer.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Context/ContextCache.hpp"
#include "Core/Context/ContextRetriever.hpp"
#include "Core/Context/SentLedger.hpp"
#include "Core/IDE/EditorStateStore.hpp"
#include "Core/Index/AstIndex.hpp"
#include "Core/Index/CallGraph.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/InheritanceGraph.hpp"
#include "Core/Index/ReferenceGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/Security/Sandbox.hpp"

#include <cstdint>
#include <istream>
#include <ostream>
#include <string>

namespace aistudio::core {

// Exposes a minimal, read-only subset of Project Intelligence / Context
// Engine to an external AI (Claude Code and similar) as MCP tools —
// docs/MASTER_SPEC.md #63 MCP Backend, per the 2026-09-01 direction
// correction recorded in CLAUDE.md: this Studio is the MCP *server*
// being connected to, not a client reaching out to LLMs itself.
//
// Speaks newline-delimited JSON-RPC 2.0 over stdio (the MCP stdio
// transport) rather than ApiServer's HTTP+CORS — a locally spawned
// child process (e.g. launched by Claude Code's own MCP client config)
// needs no network exposure or Authentication, keeping this consistent
// with docs/ROADMAP.md deferring ApiServer's Authentication to Phase 10
// (remote/multi-client) rather than needing it now.
//
// Tool set started minimal (docs/ROADMAP.md Next Priority): symbol_search
// / keyword_search / context_retrieve, all read-only. A second pass
// (docs/ROADMAP.md Next Priority "MCPサーバーの拡張") added the four
// Phase 3 graph indexes and Impact Analysis as their own tools:
// include_graph / call_graph / inheritance_graph / reference_graph /
// impact_analysis. A third pass adds ast_tree, exposing AstIndex (the one
// Phase 3 index the first two passes left unexposed even though its own
// GET /api/ast has existed in ApiServer since Phase 3). Unlike
// ContextRetriever's own internal Symbol/File/Keyword retrieval (which
// already applies `firewall` before turning a match into a ContextItem),
// every tool here that isn't context_retrieve bypasses ContextRetriever
// entirely and returns raw index data — so McpServer applies `firewall`
// to their results itself, the same Context Firewall guarantee
// (docs/MASTER_SPEC.md #21) applied a second time at this boundary.
// context_retrieve is filtered independently, by whatever firewall the
// `context_retriever` it's given was itself constructed with. A later
// pass adds active_document (docs/ROADMAP.md "## IDE"), the first tool
// that answers "what is the user currently looking at" rather than
// something about the project's own static code -- it filters its
// single result through `firewall` the same way, but EditorStateStore
// itself (not McpServer) owns that check (see EditorStateStore.hpp).
struct McpServerOptions {
    const SymbolIndex* symbol_index = nullptr;
    const ContextRetriever* context_retriever = nullptr;
    // Optional (docs/ROADMAP.md Phase 2 "Context Cache"): when set,
    // context_retrieve memoizes Retrieve() results through this cache
    // instead of calling context_retriever->Retrieve() directly every
    // time. Non-const pointer (unlike every other field here) because
    // GetOrRetrieve() mutates cache state -- CallTool() itself still only
    // ever sees `const McpServerOptions&`, so this is the same "shallow
    // const" shape ApiServer's own mutable-through-a-const-struct fields
    // use elsewhere.
    //
    // nullptr (the default) preserves the exact pre-existing behavior:
    // every call re-runs Retrieve() fresh. RunMcpMode() (Core/src/main.cpp)
    // now passes a real ContextCache (docs/ROADMAP.md Next Priority #10,
    // "RunMcpMode()へFileWatcher...を追加") -- it previously left this
    // nullptr because that mode built SymbolIndex/IncludeGraph/etc. once
    // at startup with no FileWatcher running for the lifetime of the
    // stdio session, so nothing would ever have called ContextCache::
    // InvalidateAll(). A cache with no invalidation trigger is exactly
    // the anti-pattern AGENT.md #7 warns against, so this field only
    // became safe to wire up once RunMcpMode() also gained its own
    // FileWatcher ("FileChanged" subscription mirroring the default HTTP
    // mode's identical block) to drive that invalidation. A caller that
    // still has no FileWatcher (or any other real invalidation trigger)
    // should keep leaving this nullptr, for the same reason.
    ContextCache* context_cache = nullptr;
    // Token budget for one context_retrieve response (docs/MASTER_SPEC.md
    // #99, docs/ROADMAP.md CE-1). 0 (default) = unlimited, the
    // pre-existing behavior. When set, candidates go through
    // ContextSelector::SelectWithCompression and what still doesn't fit
    // comes back as a content-free stub in "omitted" (fetchable via
    // context_fetch). Charged against the serialized JSON entry, not
    // ContextItem::estimated_tokens -- see ResponseItemCost().
    // Opt-in via `mcp.context_response_budget_tokens`.
    std::int64_t context_response_budget_tokens = 0;
    // Session-scoped dedup (docs/MASTER_SPEC.md #99, docs/ROADMAP.md
    // CE-4 "セッション既送信台帳"). false (default) preserves the exact
    // pre-existing response shape. When true and `sent_ledger` is set, a
    // context_retrieve item whose content exactly matches what this
    // session already sent for that id comes back as a content-free
    // {"id", "unchanged_since", "hint"} stub instead of resending the
    // body -- context_fetch stays the always-available way to read it
    // again regardless of this setting. Opt-in via
    // `mcp.suppress_resent_content`, because a connecting AI that already
    // compressed/discarded its own earlier context can't reconstruct
    // content from a stub alone.
    bool suppress_resent_content = false;
    // Non-const like `context_cache` above, for the same reason (mutated
    // by context_retrieve). nullptr disables the stub behavior regardless
    // of `suppress_resent_content`, and is what RunMcpMode() leaves this
    // when the config flag is off.
    SentLedger* sent_ledger = nullptr;
    const IncludeGraph* include_graph = nullptr;
    const CallGraph* call_graph = nullptr;
    const InheritanceGraph* inheritance_graph = nullptr;
    const ReferenceGraph* reference_graph = nullptr;
    // nullptr disables the ast_tree tool.
    const AstIndex* ast_index = nullptr;
    // Needs include_graph/call_graph/symbol_index to also be set (see
    // ImpactAnalyzer's own constructor) -- nullptr disables the
    // impact_analysis tool regardless of whether those three are set.
    const ImpactAnalyzer* impact_analyzer = nullptr;
    // Needs impact_analyzer to also be set -- nullptr disables the
    // changed_impact_analysis tool. Unlike every other field here, this
    // one is consulted at REQUEST time (a RunQuery("core.git",
    // "git.diff") call), not just read from at startup -- the working
    // tree can change between server startup and a tool call, so the
    // diff has to be fetched fresh each time rather than precomputed
    // once. Every other tool's data source stays startup-only.
    const BackendRegistry* backend_registry = nullptr;
    // Gates whether git_commit/git_branch/git_stash (core.git's write
    // Commands -- see GitBackend::Dispatch) are exposed as MCP tools at
    // all. Defaults to false: unlike the HTTP
    // POST /api/backends/core.git/commands route (always reachable,
    // gated only by PermissionPolicy/ApprovalQueue), an MCP client like
    // Claude Code has its OWN permission-mode gate in front of every
    // tool call, so this flag exists purely as an opt-in -- an operator
    // has to set `mcp.enable_git_write_commands=true` in aistudio.config
    // (see RunMcpMode() in Core/src/main.cpp) before these tools even
    // appear in tools/list. No effect on the HTTP path. Requires
    // backend_registry to also be set.
    bool enable_git_write_commands = false;
    // nullptr disables the active_document tool -- docs/ROADMAP.md
    // "## IDE" Active document / Selection checklist items. Reads the
    // small JSON side-channel file a connected IDE extension (currently
    // just Tools/vscode-extension) writes on every active-editor/
    // selection change; see EditorStateStore's own doc comment for why
    // this is a file rather than a push endpoint (this Studio's `--lsp`
    // and `--mcp` modes are separate processes -- an IDE extension
    // connected to one has nothing to push to in the other).
    const EditorStateStore* editor_state_store = nullptr;
    // For KeywordSearch, which (like ContextRetriever's own keyword path)
    // has no persistent index of its own to point at — empty disables
    // the keyword_search tool.
    std::string project_root;
    // nullptr applies no additional filtering to symbol_search /
    // keyword_search / graph tool results.
    const Sandbox* firewall = nullptr;
    std::string server_name = "aistudio-core";
    std::string server_version = "0.1.0";
};

class McpServer {
public:
    explicit McpServer(McpServerOptions options) : options_(std::move(options)) {}

    // Reads newline-delimited JSON-RPC 2.0 requests/notifications from
    // `in` and writes responses to `out` until `in` reaches EOF (or a
    // fatal read error). Blocking, single-threaded — matches the stdio
    // transport's own framing (one message per line, no concurrent
    // pipelining expected). Notifications (no "id") never produce a
    // response line, per JSON-RPC 2.0.
    void Run(std::istream& in, std::ostream& out) const;

private:
    McpServerOptions options_;
};

} // namespace aistudio::core
