#pragma once

#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Database/ContextAuditRepository.hpp"
#include "Core/Database/ContextSnapshotRepository.hpp"
#include "Core/Database/TaskRepository.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Index/AstIndex.hpp"
#include "Core/Index/CallGraph.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/InheritanceGraph.hpp"
#include "Core/Index/ReferenceGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/LLM/ILLMProvider.hpp"
#include "Core/Plugin/PluginRegistry.hpp"
#include "Core/Security/ApprovalQueue.hpp"
#include "Core/Security/PermissionPolicy.hpp"
#include "Core/Security/Sandbox.hpp"

#include <memory>
#include <string>

namespace httplib {
class Server;
}

namespace aistudio::core {

// The optional Project Intelligence / LLM integrations ApiServer can
// read from (docs/ROADMAP.md Phase 3, Phase 4). Every member defaults to
// nullptr/empty, meaning its endpoint (/api/plugins, /api/symbols,
// /api/includes, /api/calls, /api/inheritance, /api/references, /api/ast,
// /api/llm/complete, /api/context/usage) reports an empty list (or 503,
// for /api/llm/complete — there's no empty-list equivalent for a chat
// completion) instead, so
// existing call sites don't need to change when they don't have a given
// piece wired up yet. Consolidated into one struct — this constructor's
// parameter list had grown to five trailing optional pointers before
// AstIndex, the exact point ApiServer's own previous doc comment flagged
// as worth fixing before adding another one.
struct ApiServerIndexes {
    PluginRegistry* plugin_registry = nullptr;
    const SymbolIndex* symbol_index = nullptr;
    const IncludeGraph* include_graph = nullptr;
    const CallGraph* call_graph = nullptr;
    const InheritanceGraph* inheritance_graph = nullptr;
    const ReferenceGraph* reference_graph = nullptr;
    const AstIndex* ast_index = nullptr;
    // Unlike the indexes above, KeywordSearch has no persistent index of
    // its own to point at (docs/ROADMAP.md "Keyword Search" — a fresh
    // scan per query) — /api/search/keyword needs the project root
    // itself instead. Empty means that endpoint always reports an empty
    // list, the same graceful-degradation convention the pointers above
    // use.
    std::string project_root;
    // docs/ROADMAP.md Phase 4 "LLM Integration" — /api/llm/complete
    // dispatches here. nullptr means that endpoint reports 503.
    ILLMProvider* llm_provider = nullptr;
    // docs/ROADMAP.md "Context UI" — /api/context/usage reads every
    // ContextAuditEntry and reshapes it via SummarizeContextUsage().
    // nullptr means that endpoint reports an empty summary (all-zero
    // counts), the same graceful-degradation convention the pointers
    // above use. Not const: ContextAuditRepository's read methods aren't
    // const-qualified (same reason plugin_registry above isn't either).
    ContextAuditRepository* context_audit_repository = nullptr;
    // docs/ROADMAP.md Phase 2 "Context Restore" -- /api/context/snapshots*
    // routes read/restore from here. nullptr means those endpoints report
    // an empty list / 404, the same graceful-degradation convention the
    // pointers above use. Not const, same reason context_audit_repository
    // above isn't either.
    ContextSnapshotRepository* context_snapshot_repository = nullptr;
    // Restoring a snapshot re-surfaces content that was pruned out once
    // already, so POST /api/context/snapshots/{id}/restore needs the same
    // Context Firewall guarantee (docs/MASTER_SPEC.md #21) live retrieval
    // gets -- passed straight through to ContextRestorer::Options::firewall
    // (see its own class comment for why that one expects this normally
    // set, unlike ContextRetriever's nullable firewall).
    const Sandbox* context_firewall = nullptr;
};

// The "Application API" layer in docs/ARCHITECTURE.md's stack (Frontend
// -> Application API -> Core -> Backend Protocol -> Backends) —
// docs/ROADMAP.md Phase 0 P1 "Core <-> Frontend communication design".
//
// A minimal HTTP+JSON surface wrapping cpp-httplib / nlohmann::json
// (vendored header-only, docs/DEPENDENCY_MANAGEMENT.md). health/backends/
// tasks are read-only; command dispatch is gated by PermissionPolicy —
// a Command PermissionPolicy flags goes to ApprovalQueue instead of
// running immediately (docs/MASTER_SPEC.md #73, #79 Remote Approval).
// Real-time push (WebSocket) and authentication are still deferred until
// a client actually needs them.
class ApiServer {
public:
    ApiServer(BackendRegistry& registry, TaskRepository& task_repository, std::string studio_name,
              ApiServerIndexes indexes = {});
    ~ApiServer();

    ApiServer(const ApiServer&) = delete;
    ApiServer& operator=(const ApiServer&) = delete;

    // Binds and blocks serving requests until Stop() is called (typically
    // from another thread, or a signal handler at the call site).
    Result<void> Listen(const std::string& host, int port);
    void Stop();

    // Mutable access so the bootstrap call site can change the autonomy
    // level or whitelist backends before Listen() starts serving.
    [[nodiscard]] PermissionPolicy& Policy() { return policy_; }
    [[nodiscard]] const ApprovalQueue& Approvals() const { return approval_queue_; }

private:
    void RegisterRoutes();

    BackendRegistry& registry_;
    TaskRepository& task_repository_;
    std::string studio_name_;
    ApiServerIndexes indexes_;
    PermissionPolicy policy_;
    ApprovalQueue approval_queue_;
    std::unique_ptr<httplib::Server> server_;
};

} // namespace aistudio::core
