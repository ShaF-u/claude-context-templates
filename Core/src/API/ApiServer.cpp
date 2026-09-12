#include "Core/API/ApiServer.hpp"

#include "Core/Analysis/ImpactAnalyzer.hpp"
#include "Core/Context/ContextRestorer.hpp"
#include "Core/Context/ContextUsageSummary.hpp"
#include "Core/Git/GitBackend.hpp"
#include "Core/Protocol/Protocol.hpp"
#include "Core/Search/KeywordSearch.hpp"
#include "Core/Search/SymbolSearch.hpp"
#include "Core/Task/Task.hpp"

// Vendored header-only third-party libraries (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since these headers aren't ours to fix.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <any>

namespace aistudio::core {

namespace {

using Json = nlohmann::json;

Json AnyToJson(const std::any& value) {
    if (!value.has_value()) {
        return nullptr;
    }
    if (const auto* s = std::any_cast<std::string>(&value)) return *s;
    if (const auto* i = std::any_cast<std::int64_t>(&value)) return *i;
    if (const auto* i = std::any_cast<int>(&value)) return *i;
    if (const auto* d = std::any_cast<double>(&value)) return *d;
    if (const auto* b = std::any_cast<bool>(&value)) return *b;
    // A Plugin Backend's Dispatch()/Handle() result (PluginBackendAdapter,
    // Core/Plugin/PluginBackendAdapter.cpp) -- the plugin returns JSON
    // directly rather than a single primitive, so it's stored here as an
    // already-parsed Json value rather than one of the primitives above.
    if (const auto* j = std::any_cast<Json>(&value)) return *j;
    // GitBackend's "git.status"/"git.history" Query results
    // (Core/Git/GitBackend.hpp) -- structured domain types, not one of
    // the primitives above, so they need their own translation here the
    // same way TaskToJson translates Task at this same API boundary.
    if (const auto* status = std::any_cast<GitStatusResult>(&value)) {
        Json entries = Json::array();
        for (const auto& entry : status->entries) {
            entries.push_back(Json{{"path", entry.path}, {"status_code", entry.status_code}});
        }
        return Json{{"branch", status->branch}, {"entries", entries}};
    }
    if (const auto* commits = std::any_cast<std::vector<GitCommitEntry>>(&value)) {
        Json array = Json::array();
        for (const auto& commit : *commits) {
            array.push_back(Json{{"sha", commit.sha}, {"date", commit.date}, {"subject", commit.subject}});
        }
        return array;
    }
    return "<unrepresentable>";
}

Json TaskToJson(const Task& task) {
    return Json{
        {"id", task.id},
        {"name", task.name},
        {"state", ToString(task.state)},
        {"progress", task.progress},
        {"retry_count", task.retry_count},
        {"max_retries", task.max_retries},
        {"depends_on", task.depends_on},
    };
}

void SetCors(httplib::Response& res) {
    // Local dev API only (127.0.0.1) — wildcard CORS is fine here; a
    // Permission/Approval layer (docs/MASTER_SPEC.md #73) gates anything
    // sensitive before this is exposed beyond localhost.
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}

Json ErrorJson(const std::string& message) {
    return Json{{"error", message}};
}

// Structured counterpart: a caller with a real Core::Error to report
// (rather than an ad-hoc validation string this file writes by hand, e.g.
// "invalid JSON body") gets `code`/`retryable` alongside the same
// top-level "error" message key the string overload above already used
// -- existing consumers reading just body["error"] see no change, this
// only adds fields.
Json ErrorJson(const Error& error) {
    Json result = Json{
        {"error", error.message},
        {"code", ToString(error.code)},
        {"retryable", error.retryable},
    };
    if (!error.cause.empty()) {
        result["cause"] = error.cause;
    }
    return result;
}

Json SymbolToJson(const Symbol& symbol) {
    return Json{
        {"name", symbol.name},
        {"kind", ToString(symbol.kind)},
        {"file_path", symbol.file_path},
        {"line", symbol.line},
        {"signature", symbol.signature},
    };
}

Json IncludeEdgeToJson(const IncludeEdge& edge) {
    return Json{
        {"from_file", edge.from_file},
        {"include_text", edge.include_text},
        {"is_system", edge.is_system},
        {"line", edge.line},
        {"resolved_path", edge.resolved_path},
    };
}

Json CallEdgeToJson(const CallEdge& edge) {
    return Json{
        {"caller_name", edge.caller_name},
        {"caller_file", edge.caller_file},
        {"line", edge.line},
        {"callee_text", edge.callee_text},
    };
}

Json InheritanceEdgeToJson(const InheritanceEdge& edge) {
    return Json{
        {"derived_name", edge.derived_name},
        {"derived_file", edge.derived_file},
        {"line", edge.line},
        {"base_name", edge.base_name},
    };
}

Json ReferenceEdgeToJson(const ReferenceEdge& edge) {
    return Json{
        {"type_name", edge.type_name},
        {"referencing_file", edge.referencing_file},
        {"line", edge.line},
    };
}

Json KeywordMatchToJson(const KeywordMatch& match) {
    return Json{
        {"file_path", match.file_path},
        {"line", match.line},
        {"text", match.text},
        {"score", match.score},
    };
}

Json SymbolMatchToJson(const SymbolMatch& match) {
    Json result = SymbolToJson(match.symbol);
    result["score"] = match.score;
    return result;
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

Json ImpactResultToJson(const ImpactResult& result) {
    Json affected_symbols = Json::array();
    for (const auto& symbol : result.affected_symbols) {
        Json callers = Json::array();
        for (const auto& caller : symbol.callers) {
            callers.push_back(CallEdgeToJson(caller));
        }
        affected_symbols.push_back(Json{
            {"symbol_name", symbol.symbol_name},
            {"callers", callers},
        });
    }
    return Json{
        {"target_file", result.target_file},
        {"affected_files", result.affected_files},
        {"affected_symbols", affected_symbols},
    };
}

// `lifecycle_state`: PluginRegistry::LifecycleState(manifest.id) from the
// same registry the manifest itself came from -- nullopt only if the id
// somehow isn't registered there (can't happen for a manifest this
// function's own caller just pulled from PluginRegistry::All(), included
// for callers elsewhere that might not have that guarantee).
Json PluginManifestToJson(const PluginManifest& manifest, std::optional<PluginLifecycleState> lifecycle_state) {
    return Json{
        {"id", manifest.id},
        {"name", manifest.name},
        {"version", manifest.version},
        {"capabilities", manifest.capabilities},
        {"provided_events", manifest.provided_events},
        {"required_permissions", manifest.required_permissions},
        {"lifecycle_state", lifecycle_state.has_value() ? ToString(*lifecycle_state) : "Unknown"},
    };
}

Json ContextUsageItemToJson(const ContextUsageItem& item) {
    return Json{
        {"item_id", item.item_id},
        {"action", ToString(item.action)},
        {"estimated_tokens", item.estimated_tokens},
        {"timestamp", item.timestamp},
        {"priority", item.priority},
    };
}

Json ContextUsageSummaryToJson(const ContextUsageSummary& summary) {
    const auto to_json_array = [](const std::vector<ContextUsageItem>& items) {
        Json list = Json::array();
        for (const auto& item : items) {
            list.push_back(ContextUsageItemToJson(item));
        }
        return list;
    };
    return Json{
        {"included", to_json_array(summary.included)},
        {"excluded", to_json_array(summary.excluded)},
        {"compressed", to_json_array(summary.compressed)},
        {"total_included_tokens", summary.total_included_tokens},
    };
}

Json ContextSnapshotToJson(const ContextSnapshot& snapshot) {
    std::vector<std::string> source_kinds;
    source_kinds.reserve(snapshot.included_item_source_kinds.size());
    for (const auto kind : snapshot.included_item_source_kinds) {
        source_kinds.push_back(ToString(kind));
    }
    return Json{
        {"id", snapshot.id},
        {"task_name", snapshot.task_name},
        {"git_commit", snapshot.git_commit},
        {"used_tokens", snapshot.used_tokens},
        {"max_tokens", snapshot.max_tokens},
        {"included_item_ids", snapshot.included_item_ids},
        // Parallel to included_item_ids -- empty for a pre-migration
        // snapshot (docs/ROADMAP.md "Context Restore"), same as the
        // domain field itself.
        {"included_item_source_kinds", source_kinds},
        {"created_at", snapshot.created_at},
    };
}

Json ContextItemToJson(const ContextItem& item) {
    return Json{
        {"id", item.id},
        {"source", ToString(item.source)},
        {"content", item.content},
        {"compression", ToString(item.compression)},
        {"priority", item.priority},
        {"estimated_tokens", item.estimated_tokens},
    };
}

Json ApprovalRequestToJson(const ApprovalRequest& request) {
    std::string status;
    switch (request.status) {
        case ApprovalStatus::Pending: status = "pending"; break;
        case ApprovalStatus::Approved: status = "approved"; break;
        case ApprovalStatus::Rejected: status = "rejected"; break;
    }
    return Json{
        {"id", request.id},
        {"backend_id", request.command.backend_id},
        {"command_name", request.command.name},
        {"status", status},
        {"created_at", request.created_at},
    };
}

ChatRole ParseChatRole(const std::string& text) {
    if (text == "system") return ChatRole::System;
    if (text == "assistant") return ChatRole::Assistant;
    return ChatRole::User;
}

Json LLMResponseToJson(const LLMResponse& response) {
    return Json{
        {"content", response.content},
        {"stop_reason", response.stop_reason},
        {"usage",
         Json{
             {"input_tokens", response.usage.input_tokens},
             {"output_tokens", response.usage.output_tokens},
         }},
    };
}

} // namespace

ApiServer::ApiServer(BackendRegistry& registry, TaskRepository& task_repository, std::string studio_name,
                      ApiServerIndexes indexes)
    : registry_(registry),
      task_repository_(task_repository),
      studio_name_(std::move(studio_name)),
      indexes_(indexes),
      server_(std::make_unique<httplib::Server>()) {
    RegisterRoutes();
}

ApiServer::~ApiServer() = default;

void ApiServer::RegisterRoutes() {
    server_->set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        SetCors(res);
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // CORS headers are already applied by the pre-routing handler above for
    // every request, including this one — setting them again here would
    // append duplicate header lines (e.g. "Access-Control-Allow-Origin: *,*"),
    // which browsers reject as an invalid CORS response.
    server_->Options(R"(.*)", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    server_->Get("/api/health", [this](const httplib::Request&, httplib::Response& res) {
        // "autonomy_level" is read-only here deliberately -- this API has
        // no authentication yet (Phase 10), and AutonomyLevel governs
        // whether Command dispatch requires approval at all, so a way to
        // *change* it over unauthenticated HTTP would let a caller bypass
        // the approval gate entirely. Reporting the value an operator
        // already set via their own aistudio.config carries none of that
        // risk -- it's not a secret, just visibility into an existing
        // local setting.
        res.set_content(
            Json{{"status", "ok"}, {"studio_name", studio_name_}, {"autonomy_level", ToString(policy_.Level())}}
                .dump(),
            "application/json");
    });

    server_->Get("/api/backends", [this](const httplib::Request& req, httplib::Response& res) {
        // `?capability=` resolves via BackendRegistry's negotiated
        // lookup (docs/ROADMAP.md "Capability System"); `?min_version=`
        // only applies alongside it. Neither param present falls back
        // to every registered backend, same as before this was added.
        const auto capability_filter = req.get_param_value("capability");
        std::vector<std::shared_ptr<IBackend>> backends;
        if (capability_filter.empty()) {
            backends = registry_.All();
        } else {
            std::optional<CapabilityVersion> min_version;
            if (const auto min_version_param = req.get_param_value("min_version"); !min_version_param.empty()) {
                min_version = CapabilityVersion::Parse(min_version_param);
            }
            backends = registry_.FindByCapability(capability_filter, min_version);
        }

        Json list = Json::array();
        for (const auto& backend : backends) {
            const auto lifecycle_state = registry_.LifecycleState(backend->Id());
            list.push_back(Json{
                {"id", backend->Id()},
                {"name", backend->Name()},
                {"version", backend->Version()},
                {"capabilities", backend->Capabilities()},
                {"lifecycle_state", lifecycle_state.has_value() ? ToString(*lifecycle_state) : "Unknown"},
                {"health", ToString(backend->Health())},
            });
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/plugins", [this](const httplib::Request&, httplib::Response& res) {
        Json list = Json::array();
        if (indexes_.plugin_registry != nullptr) {
            for (const auto& manifest : indexes_.plugin_registry->All()) {
                list.push_back(PluginManifestToJson(manifest, indexes_.plugin_registry->LifecycleState(manifest.id)));
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/context/usage", [this](const httplib::Request&, httplib::Response& res) {
        ContextUsageSummary summary;
        if (indexes_.context_audit_repository != nullptr) {
            if (const auto entries_result = indexes_.context_audit_repository->FindAll(); entries_result) {
                summary = SummarizeContextUsage(entries_result.Value());
            }
        }
        res.set_content(ContextUsageSummaryToJson(summary).dump(), "application/json");
    });

    server_->Get("/api/context/snapshots", [this](const httplib::Request&, httplib::Response& res) {
        Json list = Json::array();
        if (indexes_.context_snapshot_repository != nullptr) {
            if (const auto result = indexes_.context_snapshot_repository->FindAll(); result) {
                for (const auto& snapshot : result.Value()) {
                    list.push_back(ContextSnapshotToJson(snapshot));
                }
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get(R"(/api/context/snapshots/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1].str();
        if (indexes_.context_snapshot_repository == nullptr) {
            res.status = 404;
            res.set_content(ErrorJson("no context snapshot found: " + id).dump(), "application/json");
            return;
        }
        const auto result = indexes_.context_snapshot_repository->FindById(id);
        if (!result || !result.Value().has_value()) {
            res.status = 404;
            res.set_content(ErrorJson("no context snapshot found: " + id).dump(), "application/json");
            return;
        }
        res.set_content(ContextSnapshotToJson(*result.Value()).dump(), "application/json");
    });

    server_->Post(R"(/api/context/snapshots/([^/]+)/restore)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string id = req.matches[1].str();
        if (indexes_.context_snapshot_repository == nullptr) {
            res.status = 404;
            res.set_content(ErrorJson("no context snapshot found: " + id).dump(), "application/json");
            return;
        }
        const auto find_result = indexes_.context_snapshot_repository->FindById(id);
        if (!find_result || !find_result.Value().has_value()) {
            res.status = 404;
            res.set_content(ErrorJson("no context snapshot found: " + id).dump(), "application/json");
            return;
        }
        const ContextRestorer restorer(ContextRestorer::Options{
            .project_root = indexes_.project_root,
            .firewall = indexes_.context_firewall,
            .symbol_index = indexes_.symbol_index,
            .include_graph = indexes_.include_graph,
        });
        Json list = Json::array();
        for (const auto& item : restorer.Restore(*find_result.Value())) {
            list.push_back(ContextItemToJson(item));
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/symbols", [this](const httplib::Request& req, httplib::Response& res) {
        Json list = Json::array();
        if (indexes_.symbol_index != nullptr) {
            const auto name_filter = req.get_param_value("name");
            const auto symbols = name_filter.empty() ? indexes_.symbol_index->All() : indexes_.symbol_index->FindByName(name_filter);
            for (const auto& symbol : symbols) {
                list.push_back(SymbolToJson(symbol));
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/search/symbols", [this](const httplib::Request& req, httplib::Response& res) {
        Json list = Json::array();
        const auto query = req.get_param_value("q");
        if (indexes_.symbol_index != nullptr && !query.empty()) {
            const SymbolSearch search;
            for (const auto& match : search.Search(*indexes_.symbol_index, query)) {
                list.push_back(SymbolMatchToJson(match));
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/search/keyword", [this](const httplib::Request& req, httplib::Response& res) {
        const auto query = req.get_param_value("q");
        if (query.empty() || indexes_.project_root.empty()) {
            res.set_content(Json::array().dump(), "application/json");
            return;
        }
        const KeywordSearch search;
        const auto result = search.Search(indexes_.project_root, query);
        if (!result) {
            res.status = 500;
            res.set_content(ErrorJson(result.Err()).dump(), "application/json");
            return;
        }
        Json list = Json::array();
        for (const auto& match : result.Value()) {
            list.push_back(KeywordMatchToJson(match));
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/includes", [this](const httplib::Request& req, httplib::Response& res) {
        Json list = Json::array();
        if (indexes_.include_graph != nullptr) {
            const auto file_filter = req.get_param_value("file");
            const auto included_by_filter = req.get_param_value("included_by");
            if (!file_filter.empty()) {
                for (const auto& edge : indexes_.include_graph->AllEdges()) {
                    if (edge.from_file == file_filter) {
                        list.push_back(IncludeEdgeToJson(edge));
                    }
                }
            } else if (!included_by_filter.empty()) {
                for (const auto& edge : indexes_.include_graph->AllEdges()) {
                    if (edge.resolved_path == included_by_filter) {
                        list.push_back(IncludeEdgeToJson(edge));
                    }
                }
            } else {
                for (const auto& edge : indexes_.include_graph->AllEdges()) {
                    list.push_back(IncludeEdgeToJson(edge));
                }
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/calls", [this](const httplib::Request& req, httplib::Response& res) {
        Json list = Json::array();
        if (indexes_.call_graph != nullptr) {
            const auto caller_filter = req.get_param_value("caller");
            const auto callee_filter = req.get_param_value("callee");
            std::vector<CallEdge> edges;
            if (!caller_filter.empty()) {
                edges = indexes_.call_graph->Callees(caller_filter);
            } else if (!callee_filter.empty()) {
                edges = indexes_.call_graph->Callers(callee_filter);
            } else {
                edges = indexes_.call_graph->AllEdges();
            }
            for (const auto& edge : edges) {
                list.push_back(CallEdgeToJson(edge));
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/inheritance", [this](const httplib::Request& req, httplib::Response& res) {
        Json list = Json::array();
        if (indexes_.inheritance_graph != nullptr) {
            const auto derived_filter = req.get_param_value("derived");
            const auto base_filter = req.get_param_value("base");
            std::vector<InheritanceEdge> edges;
            if (!derived_filter.empty()) {
                edges = indexes_.inheritance_graph->Bases(derived_filter);
            } else if (!base_filter.empty()) {
                edges = indexes_.inheritance_graph->Derived(base_filter);
            } else {
                edges = indexes_.inheritance_graph->AllEdges();
            }
            for (const auto& edge : edges) {
                list.push_back(InheritanceEdgeToJson(edge));
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/references", [this](const httplib::Request& req, httplib::Response& res) {
        Json list = Json::array();
        if (indexes_.reference_graph != nullptr) {
            const auto type_filter = req.get_param_value("type");
            const auto edges = type_filter.empty() ? indexes_.reference_graph->AllEdges() : indexes_.reference_graph->References(type_filter);
            for (const auto& edge : edges) {
                list.push_back(ReferenceEdgeToJson(edge));
            }
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Get("/api/ast", [this](const httplib::Request& req, httplib::Response& res) {
        const auto file_filter = req.get_param_value("file");
        if (file_filter.empty()) {
            res.status = 400;
            res.set_content(ErrorJson("missing required query param: file").dump(), "application/json");
            return;
        }
        if (indexes_.ast_index == nullptr) {
            res.set_content(Json(nullptr).dump(), "application/json");
            return;
        }
        const auto tree = indexes_.ast_index->Get(file_filter);
        if (!tree.has_value()) {
            res.status = 404;
            res.set_content(ErrorJson("no AST indexed for file: " + file_filter).dump(), "application/json");
            return;
        }
        res.set_content(AstNodeToJson(*tree).dump(), "application/json");
    });

    server_->Get("/api/impact", [this](const httplib::Request& req, httplib::Response& res) {
        const auto file_filter = req.get_param_value("file");
        if (file_filter.empty()) {
            res.status = 400;
            res.set_content(ErrorJson("missing required query param: file").dump(), "application/json");
            return;
        }
        if (indexes_.include_graph == nullptr || indexes_.call_graph == nullptr || indexes_.symbol_index == nullptr) {
            res.set_content(ImpactResultToJson(ImpactResult{.target_file = file_filter}).dump(), "application/json");
            return;
        }
        const ImpactAnalyzer analyzer(*indexes_.include_graph, *indexes_.call_graph, *indexes_.symbol_index);
        res.set_content(ImpactResultToJson(analyzer.Analyze(file_filter)).dump(), "application/json");
    });

    server_->Post("/api/llm/complete", [this](const httplib::Request& req, httplib::Response& res) {
        if (indexes_.llm_provider == nullptr) {
            res.status = 503;
            res.set_content(ErrorJson("no LLM provider configured").dump(), "application/json");
            return;
        }

        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const Json::parse_error&) {
            res.status = 400;
            res.set_content(ErrorJson("invalid JSON body").dump(), "application/json");
            return;
        }

        LLMRequest request;
        request.model = body.value("model", "");
        request.max_tokens = body.value("max_tokens", 1024);
        if (body.contains("messages") && body["messages"].is_array()) {
            for (const auto& item : body["messages"]) {
                ChatMessage message;
                message.role = ParseChatRole(item.value("role", "user"));
                message.content = item.value("content", "");
                request.messages.push_back(std::move(message));
            }
        }

        const auto result = indexes_.llm_provider->Complete(request);
        if (!result) {
            res.status = 400;
            res.set_content(ErrorJson(result.Err()).dump(), "application/json");
            return;
        }
        res.set_content(LLMResponseToJson(result.Value()).dump(), "application/json");
    });

    server_->Post(R"(/api/backends/([^/]+)/commands)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string backend_id = req.matches[1].str();

        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const Json::parse_error&) {
            res.status = 400;
            res.set_content(ErrorJson("invalid JSON body").dump(), "application/json");
            return;
        }

        Command command;
        command.backend_id = backend_id;
        command.name = body.value("name", "");
        if (body.contains("payload") && body["payload"].is_string()) {
            command.payload = body["payload"].get<std::string>();
        }

        if (policy_.RequiresApproval(command)) {
            const auto approval_id = approval_queue_.Submit(command);
            res.status = 202;
            res.set_content(Json{{"status", "pending_approval"}, {"approval_id", approval_id}}.dump(),
                             "application/json");
            return;
        }

        const auto result = registry_.Dispatch(command);
        if (!result) {
            res.status = 404;
            res.set_content(ErrorJson(result.Err()).dump(), "application/json");
            return;
        }
        res.set_content(Json{{"ok", true}, {"result", AnyToJson(result.Value())}}.dump(), "application/json");
    });

    // Read-only counterpart of POST .../commands above -- BackendRegistry::
    // RunQuery() has existed and been tested since Phase 0, but nothing
    // ever exposed it over HTTP (only the MCP server's tools and direct
    // in-process callers could reach it). No PermissionPolicy/
    // ApprovalQueue gate here: Query::parameters's own doc comment says
    // "Never mutates Backend state", and neither PermissionPolicy::
    // RequiresApproval() nor ApprovalQueue::Submit() even accept a Query
    // (Command-only) -- there is nothing to gate. "payload" is the JSON
    // key here too (not "parameters", Query's own C++ field name) to
    // match POST .../commands's wire vocabulary and PluginBackendAdapter::
    // Handle()'s own query_json shape, rather than mirroring the C++
    // field name.
    server_->Post(R"(/api/backends/([^/]+)/queries)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string backend_id = req.matches[1].str();

        Json body;
        try {
            body = Json::parse(req.body);
        } catch (const Json::parse_error&) {
            res.status = 400;
            res.set_content(ErrorJson("invalid JSON body").dump(), "application/json");
            return;
        }

        Query query;
        query.backend_id = backend_id;
        query.name = body.value("name", "");
        if (body.contains("payload") && body["payload"].is_string()) {
            query.parameters = body["payload"].get<std::string>();
        }

        const auto result = registry_.RunQuery(query);
        if (!result) {
            res.status = 404;
            res.set_content(ErrorJson(result.Err()).dump(), "application/json");
            return;
        }
        res.set_content(Json{{"ok", true}, {"result", AnyToJson(result.Value())}}.dump(), "application/json");
    });

    // BackendRegistry::StartBackend()/StopBackend() have existed and
    // been tested since the Backend Lifecycle state machine was added,
    // but -- same gap as RunQuery() before it -- nothing ever exposed
    // them over HTTP; GET /api/backends could only ever report
    // lifecycle_state, never change it. No PermissionPolicy/
    // ApprovalQueue gate here either: those only ever gated Command
    // dispatch, and starting/stopping a Backend was never modeled as a
    // Command -- this is exactly the same "nothing to gate" situation
    // POST .../queries's own comment already explains, just for a
    // different action. Every failure here (unknown id, or an invalid
    // state transition e.g. starting an already-Running backend) returns
    // 404, matching every other Result<T>-backed route in this file
    // (see POST .../commands, .../queries, .../approvals/.../approve) --
    // ErrorJson(const Error&)'s own "code" field (NotFound vs
    // InvalidArgument here) is what actually distinguishes the reason,
    // not the HTTP status.
    server_->Post(R"(/api/backends/([^/]+)/start)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string backend_id = req.matches[1].str();
        const auto result = registry_.StartBackend(backend_id);
        if (!result) {
            res.status = 404;
            res.set_content(ErrorJson(result.Err()).dump(), "application/json");
            return;
        }
        res.set_content(Json{{"ok", true}}.dump(), "application/json");
    });

    server_->Post(R"(/api/backends/([^/]+)/stop)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string backend_id = req.matches[1].str();
        const auto result = registry_.StopBackend(backend_id);
        if (!result) {
            res.status = 404;
            res.set_content(ErrorJson(result.Err()).dump(), "application/json");
            return;
        }
        res.set_content(Json{{"ok", true}}.dump(), "application/json");
    });

    server_->Get("/api/approvals", [this](const httplib::Request&, httplib::Response& res) {
        Json list = Json::array();
        for (const auto& request : approval_queue_.Pending()) {
            list.push_back(ApprovalRequestToJson(request));
        }
        res.set_content(list.dump(), "application/json");
    });

    server_->Post(R"(/api/approvals/([^/]+)/approve)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string approval_id = req.matches[1].str();

        const auto approve_result = approval_queue_.Approve(approval_id);
        if (!approve_result) {
            res.status = 404;
            res.set_content(ErrorJson(approve_result.Err()).dump(), "application/json");
            return;
        }

        const auto request = approval_queue_.Find(approval_id);
        const auto dispatch_result = registry_.Dispatch(request->command);
        approval_queue_.Remove(approval_id);

        if (!dispatch_result) {
            res.status = 404;
            res.set_content(ErrorJson(dispatch_result.Err()).dump(), "application/json");
            return;
        }
        res.set_content(Json{{"ok", true}, {"result", AnyToJson(dispatch_result.Value())}}.dump(), "application/json");
    });

    server_->Post(R"(/api/approvals/([^/]+)/reject)", [this](const httplib::Request& req, httplib::Response& res) {
        const std::string approval_id = req.matches[1].str();

        const auto reject_result = approval_queue_.Reject(approval_id);
        if (!reject_result) {
            res.status = 404;
            res.set_content(ErrorJson(reject_result.Err()).dump(), "application/json");
            return;
        }
        approval_queue_.Remove(approval_id);
        res.set_content(Json{{"status", "rejected"}}.dump(), "application/json");
    });

    server_->Get("/api/tasks", [this](const httplib::Request&, httplib::Response& res) {
        const auto result = task_repository_.FindAll();
        if (!result) {
            res.status = 500;
            res.set_content(ErrorJson(result.Err()).dump(), "application/json");
            return;
        }
        Json list = Json::array();
        for (const auto& task : result.Value()) {
            list.push_back(TaskToJson(task));
        }
        res.set_content(list.dump(), "application/json");
    });
}

Result<void> ApiServer::Listen(const std::string& host, int port) {
    if (!server_->bind_to_port(host, port)) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to bind " + host + ":" + std::to_string(port),
            .module = "Core.API",
        });
    }
    server_->listen_after_bind();
    return Result<void>::Ok();
}

void ApiServer::Stop() {
    server_->stop();
}

} // namespace aistudio::core
