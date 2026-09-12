#include "test_framework.hpp"
#include "Core/API/ApiServer.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Database/ContextAuditRepository.hpp"
#include "Core/Database/ContextSnapshotRepository.hpp"
#include "Core/Database/Database.hpp"
#include "Core/Database/TaskRepository.hpp"
#include "Core/Git/GitBackend.hpp"
#include "Core/Index/AstIndex.hpp"
#include "Core/Index/CallGraph.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/InheritanceGraph.hpp"
#include "Core/Index/ReferenceGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/LLM/EchoLLMProvider.hpp"
#include "Core/Plugin/PluginRegistry.hpp"
#include "Core/Security/Sandbox.hpp"
#include "Core/Util/ProcessRunner.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <httplib.h>
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>

using namespace aistudio::core;

namespace {

using Json = nlohmann::json;

class FakeApiBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "fake"; }
    [[nodiscard]] std::string Name() const override { return "Fake"; }
    [[nodiscard]] std::string Version() const override { return "0.0.1"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override {
        return {"fake.capability", "versioned.capability@2.1.0"};
    }
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }

    CommandResult Dispatch(const Command& command) override {
        if (command.name == "echo") {
            return CommandResult::Ok(command.payload);
        }
        return CommandResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unknown command: " + command.name});
    }

    QueryResult Handle(const Query& query) override {
        if (query.name == "echo") {
            return QueryResult::Ok(query.parameters);
        }
        return QueryResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unknown query: " + query.name});
    }
};

// Binds, starts serving on a background thread, and stops/joins on
// destruction — so each test gets an isolated live server instead of
// mocking HTTP away (this is the layer Frontend actually talks to).
struct RunningServer {
    Database db;
    ContextAuditRepository context_audit_repository;
    ContextSnapshotRepository context_snapshot_repository;
    Sandbox firewall;
    BackendRegistry registry;
    PluginRegistry plugin_registry;
    SymbolIndex symbol_index;
    IncludeGraph include_graph;
    CallGraph call_graph;
    InheritanceGraph inheritance_graph;
    ReferenceGraph reference_graph;
    AstIndex ast_index;
    EchoLLMProvider llm_provider;
    std::unique_ptr<TaskRepository> task_repository;
    std::unique_ptr<ApiServer> api;
    std::thread thread;
    int port;

    explicit RunningServer(int listen_port, std::string project_root = "")
        : context_audit_repository(db), context_snapshot_repository(db), firewall(project_root), port(listen_port) {
        db.Open(":memory:");
        context_audit_repository.EnsureSchema();
        context_snapshot_repository.EnsureSchema();
        task_repository = std::make_unique<TaskRepository>(db);
        task_repository->EnsureSchema();
        registry.Register(std::make_shared<FakeApiBackend>());
        api = std::make_unique<ApiServer>(registry, *task_repository, "Test Studio",
                                           ApiServerIndexes{
                                               .plugin_registry = &plugin_registry,
                                               .symbol_index = &symbol_index,
                                               .include_graph = &include_graph,
                                               .call_graph = &call_graph,
                                               .inheritance_graph = &inheritance_graph,
                                               .reference_graph = &reference_graph,
                                               .ast_index = &ast_index,
                                               .project_root = std::move(project_root),
                                               .llm_provider = &llm_provider,
                                               .context_audit_repository = &context_audit_repository,
                                               .context_snapshot_repository = &context_snapshot_repository,
                                               .context_firewall = &firewall,
                                           });
        thread = std::thread([this] { api->Listen("127.0.0.1", port); });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    ~RunningServer() {
        api->Stop();
        thread.join();
    }
};

} // namespace

AISTUDIO_TEST(ApiServer_Health_ReturnsStudioName) {
    RunningServer server(18081);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/health");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["status"] == "ok");
    AISTUDIO_EXPECT(body["studio_name"] == "Test Studio");
    AISTUDIO_EXPECT(body["autonomy_level"] == "Assisted"); // ApiServer's own PermissionPolicy default
}

AISTUDIO_TEST(ApiServer_Health_ReflectsChangedAutonomyLevel) {
    RunningServer server(18142);
    server.api->Policy().SetAutonomyLevel(AutonomyLevel::Autonomous);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/health");
    AISTUDIO_EXPECT(Json::parse(res->body)["autonomy_level"] == "Autonomous");
}

AISTUDIO_TEST(ApiServer_Backends_ListsRegisteredBackend) {
    RunningServer server(18082);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/backends");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.is_array());
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["id"] == "fake");
    AISTUDIO_EXPECT(body[0]["capabilities"][0] == "fake.capability");
    AISTUDIO_EXPECT(body[0]["lifecycle_state"] == "Registered"); // RunningServer registers it but never starts it
    AISTUDIO_EXPECT(body[0]["health"] == "Healthy"); // FakeApiBackend::Health()'s own fixed return value
}

AISTUDIO_TEST(ApiServer_Backends_ReflectsRunningLifecycleStateAfterStart) {
    RunningServer server(18136);
    server.registry.StartBackend("fake");

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/backends");
    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body[0]["lifecycle_state"] == "Running");
}

AISTUDIO_TEST(ApiServer_Backends_FilterByCapability_MatchesRegardlessOfVersion) {
    RunningServer server(18116);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/backends?capability=versioned.capability");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["id"] == "fake");
}

AISTUDIO_TEST(ApiServer_Backends_FilterByCapability_UnknownCapability_ReturnsEmpty) {
    RunningServer server(18117);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/backends?capability=does.not.exist");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_Backends_FilterByCapabilityAndCompatibleMinVersion_Matches) {
    RunningServer server(18118);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/backends?capability=versioned.capability&min_version=2.0.0");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(Json::parse(res->body).size() == 1);
}

AISTUDIO_TEST(ApiServer_Backends_FilterByCapabilityAndIncompatibleMinVersion_ReturnsEmpty) {
    RunningServer server(18119);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/backends?capability=versioned.capability&min_version=3.0.0");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_Commands_DispatchesToBackend) {
    RunningServer server(18083);
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "echo"}, {"payload", "hello"}};
    const auto res = client.Post("/api/backends/fake/commands", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["ok"] == true);
    AISTUDIO_EXPECT(body["result"] == "hello");
}

AISTUDIO_TEST(ApiServer_Commands_UnknownBackend_Returns404) {
    RunningServer server(18084);
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "echo"}};
    const auto res = client.Post("/api/backends/does-not-exist/commands", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);

    // A real Core::Error backs this failure (BackendRegistry::Dispatch's
    // own NotFound) -- the structured ErrorJson(const Error&) overload
    // applies, unlike the hand-written validation strings elsewhere in
    // this file (see ApiServer_Commands_InvalidJson_Returns400 below).
    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["error"].get<std::string>().find("does-not-exist") != std::string::npos);
    AISTUDIO_EXPECT(body["code"] == "NotFound");
    AISTUDIO_EXPECT(body["retryable"] == false);
}

AISTUDIO_TEST(ApiServer_Commands_InvalidJson_Returns400) {
    RunningServer server(18085);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Post("/api/backends/fake/commands", "not json", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 400);

    // No Core::Error backs this failure (it's a hand-written validation
    // string, not something a Result<T> produced) -- the plain
    // ErrorJson(const std::string&) overload applies, so there's no
    // "code"/"retryable" to check, only "error".
    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.contains("error"));
    AISTUDIO_EXPECT(!body.contains("code"));
}

AISTUDIO_TEST(ApiServer_Queries_HandledByBackend) {
    RunningServer server(18130);
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "echo"}, {"payload", "hello"}};
    const auto res = client.Post("/api/backends/fake/queries", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["ok"] == true);
    AISTUDIO_EXPECT(body["result"] == "hello");
}

AISTUDIO_TEST(ApiServer_Queries_UnknownBackend_Returns404) {
    RunningServer server(18131);
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "echo"}};
    const auto res = client.Post("/api/backends/does-not-exist/queries", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
}

AISTUDIO_TEST(ApiServer_Queries_InvalidJson_Returns400) {
    RunningServer server(18132);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Post("/api/backends/fake/queries", "not json", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 400);
}

AISTUDIO_TEST(ApiServer_Queries_UnhandledQueryName_Returns404) {
    RunningServer server(18133);
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "not_implemented"}};
    const auto res = client.Post("/api/backends/fake/queries", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
}

AISTUDIO_TEST(ApiServer_Queries_NeverRequiresApproval_EvenForDangerousLookingName) {
    // Query::parameters's own doc comment: "Never mutates Backend state"
    // -- there is no PermissionPolicy/ApprovalQueue gate for queries at
    // all (see the route's own comment in ApiServer.cpp), so even a name
    // that would match a Command dangerous-pattern (e.g. "*.delete")
    // dispatches immediately rather than coming back 202 pending_approval.
    RunningServer server(18134);
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "resource.delete"}};
    const auto res = client.Post("/api/backends/fake/queries", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status != 202);
}

AISTUDIO_TEST(ApiServer_BackendStart_StartsRegisteredBackend) {
    RunningServer server(18137);
    httplib::Client client("127.0.0.1", server.port);

    const auto start_res = client.Post("/api/backends/fake/start", "", "application/json");
    AISTUDIO_EXPECT(start_res != nullptr);
    AISTUDIO_EXPECT(start_res->status == 200);
    AISTUDIO_EXPECT(Json::parse(start_res->body)["ok"] == true);

    const auto list_res = client.Get("/api/backends");
    AISTUDIO_EXPECT(Json::parse(list_res->body)[0]["lifecycle_state"] == "Running");
}

AISTUDIO_TEST(ApiServer_BackendStart_UnknownBackend_Returns404WithNotFoundCode) {
    RunningServer server(18138);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Post("/api/backends/does-not-exist/start", "", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
    AISTUDIO_EXPECT(Json::parse(res->body)["code"] == "NotFound");
}

AISTUDIO_TEST(ApiServer_BackendStart_AlreadyRunning_Returns404WithInvalidArgumentCode) {
    RunningServer server(18139);
    httplib::Client client("127.0.0.1", server.port);

    client.Post("/api/backends/fake/start", "", "application/json");
    const auto res = client.Post("/api/backends/fake/start", "", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
    AISTUDIO_EXPECT(Json::parse(res->body)["code"] == "InvalidArgument");
}

AISTUDIO_TEST(ApiServer_BackendStop_StopsRunningBackend) {
    RunningServer server(18140);
    httplib::Client client("127.0.0.1", server.port);

    client.Post("/api/backends/fake/start", "", "application/json");
    const auto stop_res = client.Post("/api/backends/fake/stop", "", "application/json");
    AISTUDIO_EXPECT(stop_res != nullptr);
    AISTUDIO_EXPECT(stop_res->status == 200);

    const auto list_res = client.Get("/api/backends");
    AISTUDIO_EXPECT(Json::parse(list_res->body)[0]["lifecycle_state"] == "Stopped");
}

AISTUDIO_TEST(ApiServer_BackendStop_NotRunning_Returns404) {
    RunningServer server(18141);
    httplib::Client client("127.0.0.1", server.port);

    // "fake" is registered but never started -- Registered can't stop.
    const auto res = client.Post("/api/backends/fake/stop", "", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
}

AISTUDIO_TEST(ApiServer_Tasks_ReturnsPersistedTasks) {
    RunningServer server(18086);

    Task task;
    task.id = "t1";
    task.name = "test task";
    task.state = TaskState::Running;
    task.progress = 0.25;
    server.task_repository->Save(task);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/tasks");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["id"] == "t1");
    AISTUDIO_EXPECT(body[0]["state"] == "Running");
}

AISTUDIO_TEST(ApiServer_Cors_HeaderPresentOnResponses) {
    RunningServer server(18087);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/health");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->has_header("Access-Control-Allow-Origin"));
    AISTUDIO_EXPECT(res->get_header_value("Access-Control-Allow-Origin") == "*");
}

// Regression test: the OPTIONS preflight handler used to call SetCors() on
// top of the pre-routing handler that already sets it, producing a
// duplicated "Access-Control-Allow-Origin: *,*" header that real browsers
// reject as an invalid CORS response.
AISTUDIO_TEST(ApiServer_Cors_PreflightDoesNotDuplicateHeaders) {
    RunningServer server(18088);
    httplib::Client client("127.0.0.1", server.port);

    httplib::Headers headers = {
        {"Origin", "http://localhost:5173"},
        {"Access-Control-Request-Method", "GET"},
    };
    const auto res = client.Options("/api/health", headers);
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 204);
    AISTUDIO_EXPECT(res->get_header_value("Access-Control-Allow-Origin") == "*");
    AISTUDIO_EXPECT(res->get_header_value_count("Access-Control-Allow-Origin") == 1);
}

AISTUDIO_TEST(ApiServer_Commands_DangerousCommand_RequiresApproval) {
    RunningServer server(18089);
    httplib::Client client("127.0.0.1", server.port);

    // "asset.delete" matches PermissionPolicy's default dangerous
    // patterns ("*.delete"), so under the default Assisted level it must
    // not dispatch immediately.
    const Json request_body{{"name", "asset.delete"}};
    const auto res = client.Post("/api/backends/fake/commands", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 202);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["status"] == "pending_approval");
    AISTUDIO_EXPECT(body.contains("approval_id"));
}

AISTUDIO_TEST(ApiServer_Approvals_ListsPendingRequest) {
    RunningServer server(18090);
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "asset.delete"}};
    client.Post("/api/backends/fake/commands", request_body.dump(), "application/json");

    const auto res = client.Get("/api/approvals");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.is_array());
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["command_name"] == "asset.delete");
    AISTUDIO_EXPECT(body[0]["status"] == "pending");
}

AISTUDIO_TEST(ApiServer_Approvals_Approve_DispatchesApprovedCommand) {
    RunningServer server(18091);
    server.api->Policy().AddDangerousPattern("echo");
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "echo"}, {"payload", "hello"}};
    const auto submit_res = client.Post("/api/backends/fake/commands", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(submit_res != nullptr);
    AISTUDIO_EXPECT(submit_res->status == 202);
    const auto submit_body = Json::parse(submit_res->body);
    const std::string approval_id = submit_body["approval_id"];

    const auto approve_res = client.Post("/api/approvals/" + approval_id + "/approve", "", "application/json");
    AISTUDIO_EXPECT(approve_res != nullptr);
    AISTUDIO_EXPECT(approve_res->status == 200);
    const auto approve_body = Json::parse(approve_res->body);
    AISTUDIO_EXPECT(approve_body["ok"] == true);
    AISTUDIO_EXPECT(approve_body["result"] == "hello");

    const auto pending_res = client.Get("/api/approvals");
    AISTUDIO_EXPECT(Json::parse(pending_res->body).empty());
}

AISTUDIO_TEST(ApiServer_Approvals_Reject_RemovesFromPendingWithoutDispatching) {
    RunningServer server(18092);
    server.api->Policy().AddDangerousPattern("echo");
    httplib::Client client("127.0.0.1", server.port);

    const Json request_body{{"name", "echo"}, {"payload", "hello"}};
    const auto submit_res = client.Post("/api/backends/fake/commands", request_body.dump(), "application/json");
    const auto submit_body = Json::parse(submit_res->body);
    const std::string approval_id = submit_body["approval_id"];

    const auto reject_res = client.Post("/api/approvals/" + approval_id + "/reject", "", "application/json");
    AISTUDIO_EXPECT(reject_res != nullptr);
    AISTUDIO_EXPECT(reject_res->status == 200);
    AISTUDIO_EXPECT(Json::parse(reject_res->body)["status"] == "rejected");

    const auto pending_res = client.Get("/api/approvals");
    AISTUDIO_EXPECT(Json::parse(pending_res->body).empty());
}

AISTUDIO_TEST(ApiServer_Approvals_Approve_UnknownId_Returns404) {
    RunningServer server(18093);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Post("/api/approvals/does-not-exist/approve", "", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
}

AISTUDIO_TEST(ApiServer_Plugins_EmptyByDefault) {
    RunningServer server(18094);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/plugins");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_Plugins_ListsRegisteredManifest) {
    RunningServer server(18095);
    PluginManifest manifest;
    manifest.id = "fake-plugin";
    manifest.name = "Fake Plugin";
    manifest.version = "1.0.0";
    manifest.capabilities = {"fake.capability"};
    server.plugin_registry.RegisterManifest(manifest);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/plugins");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["id"] == "fake-plugin");
    AISTUDIO_EXPECT(body[0]["capabilities"][0] == "fake.capability");
    AISTUDIO_EXPECT(body[0]["lifecycle_state"] == "Registered"); // never Enabled/Disabled, RegisterManifest's own default
}

AISTUDIO_TEST(ApiServer_Plugins_ReflectsEnabledLifecycleState) {
    RunningServer server(18135);
    PluginManifest manifest;
    manifest.id = "fake-plugin";
    manifest.name = "Fake Plugin";
    manifest.version = "1.0.0";
    server.plugin_registry.RegisterManifest(manifest);
    server.plugin_registry.EnablePlugin("fake-plugin");

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/plugins");
    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body[0]["lifecycle_state"] == "Enabled");
}

AISTUDIO_TEST(ApiServer_Symbols_EmptyByDefault) {
    RunningServer server(18096);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/symbols");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_Symbols_ListsIndexedSymbols) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_symbol_test_list";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "Player.hpp", std::ios::binary);
        out << "class Player {\n};\n";
    }

    RunningServer server(18097);
    server.symbol_index.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/symbols");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["name"] == "Player");
    AISTUDIO_EXPECT(body[0]["kind"] == "Class");

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_Symbols_FilterByNameQueryParam) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_symbol_test_filter";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.hpp", std::ios::binary);
        out << "class Foo {\n};\nstruct Bar {\n};\n";
    }

    RunningServer server(18098);
    server.symbol_index.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/symbols?name=Foo");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["name"] == "Foo");

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_Includes_EmptyByDefault) {
    RunningServer server(18099);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/includes");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_Includes_ListsResolvedEdges) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_include_test_list";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir / "include" / "Foo");
    {
        std::ofstream header(temp_dir / "include" / "Foo" / "Bar.hpp", std::ios::binary);
        header << "struct Bar {};\n";
        std::ofstream source(temp_dir / "main.cpp", std::ios::binary);
        source << "#include \"Foo/Bar.hpp\"\n";
    }

    RunningServer server(18100);
    server.include_graph.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/includes?file=main.cpp");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["from_file"] == "main.cpp");
    AISTUDIO_EXPECT(body[0]["resolved_path"] == "include/Foo/Bar.hpp");

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_Calls_EmptyByDefault) {
    RunningServer server(18101);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/calls");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_Calls_FilterByCallerQueryParam) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_call_test_filter";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.cpp", std::ios::binary);
        out << "void Foo() {\n    Bar();\n}\n";
    }

    RunningServer server(18102);
    server.call_graph.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/calls?caller=Foo");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["caller_name"] == "Foo");
    AISTUDIO_EXPECT(body[0]["callee_text"] == "Bar");

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_Inheritance_EmptyByDefault) {
    RunningServer server(18105);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/inheritance");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_Inheritance_FilterByDerivedQueryParam) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_inheritance_test_filter";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.hpp", std::ios::binary);
        out << "class Foo : public Bar {\n};\n";
    }

    RunningServer server(18106);
    server.inheritance_graph.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/inheritance?derived=Foo");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["derived_name"] == "Foo");
    AISTUDIO_EXPECT(body[0]["base_name"] == "Bar");

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_References_EmptyByDefault) {
    RunningServer server(18107);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/references");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_References_FilterByTypeQueryParam) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_reference_test_filter";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.hpp", std::ios::binary);
        out << "class Foo {\n    Bar b;\n};\n";
    }

    RunningServer server(18108);
    server.reference_graph.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/references?type=Bar");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["type_name"] == "Bar");
    AISTUDIO_EXPECT(body[0]["referencing_file"] == "a.hpp");

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_Impact_MissingFileParam_Returns400) {
    RunningServer server(18103);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/impact");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 400);
}

AISTUDIO_TEST(ApiServer_Impact_ReturnsAffectedFilesAndSymbols) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_impact_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream a(temp_dir / "a.hpp", std::ios::binary);
        a << "void Foo();\n";
        std::ofstream a_impl(temp_dir / "a.cpp", std::ios::binary);
        a_impl << "void Foo() {\n}\n";
        std::ofstream b(temp_dir / "b.cpp", std::ios::binary);
        b << "#include \"a.hpp\"\nvoid Caller() {\n    Foo();\n}\n";
    }

    RunningServer server(18104);
    server.include_graph.Build(temp_dir.string());
    server.call_graph.Build(temp_dir.string());
    server.symbol_index.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/impact?file=a.hpp");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["target_file"] == "a.hpp");
    AISTUDIO_EXPECT(body["affected_files"].size() == 1);
    AISTUDIO_EXPECT(body["affected_files"][0] == "b.cpp");

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_Ast_MissingFileParam_Returns400) {
    RunningServer server(18109);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/ast");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 400);
}

AISTUDIO_TEST(ApiServer_Ast_UnknownFile_Returns404) {
    RunningServer server(18110);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/ast?file=does_not_exist.hpp");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
}

AISTUDIO_TEST(ApiServer_Ast_ReturnsTreeForIndexedFile) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_ast_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.hpp", std::ios::binary);
        out << "class Foo {\n};\n";
    }

    RunningServer server(18111);
    server.ast_index.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/ast?file=a.hpp");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["kind"] == "translation_unit");
    AISTUDIO_EXPECT(!body["children"].empty());

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_SearchSymbols_EmptyQuery_ReturnsEmpty) {
    RunningServer server(18112);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/search/symbols");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_SearchSymbols_ReturnsRankedMatches) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_search_symbols_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.hpp", std::ios::binary);
        out << "class Attack {\n};\nclass AttackComponent {\n};\n";
    }

    RunningServer server(18113);
    server.symbol_index.Build(temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/search/symbols?q=Attack");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 2);
    AISTUDIO_EXPECT(body[0]["name"] == "Attack");
    AISTUDIO_EXPECT(body[0]["score"] > body[1]["score"]);

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_SearchKeyword_NoProjectRoot_ReturnsEmpty) {
    RunningServer server(18114);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/search/keyword?q=attack");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body).empty());
}

AISTUDIO_TEST(ApiServer_SearchKeyword_ReturnsMatchingLines) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_search_keyword_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.cpp", std::ios::binary);
        out << "void Foo() {\n    DoAttack();\n}\n";
    }

    RunningServer server(18115, temp_dir.string());

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/search/keyword?q=attack");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["file_path"] == "a.cpp");
    AISTUDIO_EXPECT(body[0]["line"] == 2);

    fs::remove_all(temp_dir);
}

AISTUDIO_TEST(ApiServer_LlmComplete_ReturnsEchoedResponse) {
    RunningServer server(18120);
    httplib::Client client("127.0.0.1", server.port);

    const Json body = {{"model", "test-model"}, {"messages", Json::array({Json{{"role", "user"}, {"content", "hello"}}})}};
    const auto res = client.Post("/api/llm/complete", body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto response_body = Json::parse(res->body);
    AISTUDIO_EXPECT(response_body["content"] == "echo: hello");
    AISTUDIO_EXPECT(response_body["usage"]["input_tokens"] > 0);
}

AISTUDIO_TEST(ApiServer_LlmComplete_InvalidJson_Returns400) {
    RunningServer server(18121);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Post("/api/llm/complete", "not json", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 400);
}

AISTUDIO_TEST(ApiServer_LlmComplete_NoMessages_Returns400) {
    RunningServer server(18122);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Post("/api/llm/complete", "{}", "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 400);
}

AISTUDIO_TEST(ApiServer_ContextUsage_NoEntries_ReturnsEmptySummary) {
    RunningServer server(18123);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/context/usage");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["included"].empty());
    AISTUDIO_EXPECT(body["excluded"].empty());
    AISTUDIO_EXPECT(body["compressed"].empty());
    AISTUDIO_EXPECT(body["total_included_tokens"] == 0);
}

AISTUDIO_TEST(ApiServer_ContextSnapshots_List_ReturnsSavedSnapshots) {
    RunningServer server(18150);

    ContextSnapshot snapshot;
    snapshot.id = "snap-1";
    snapshot.task_name = "test task";
    snapshot.used_tokens = 42;
    snapshot.max_tokens = 100;
    snapshot.included_item_ids = {"a.cpp", "b.cpp"};
    snapshot.included_item_source_kinds = {ContextSourceKind::File, ContextSourceKind::File};
    snapshot.created_at = 1700000000;
    server.context_snapshot_repository.Save(snapshot);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/context/snapshots");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["id"] == "snap-1");
    AISTUDIO_EXPECT(body[0]["task_name"] == "test task");
    AISTUDIO_EXPECT(body[0]["used_tokens"] == 42);
    AISTUDIO_EXPECT(body[0]["included_item_ids"].size() == 2);
    AISTUDIO_EXPECT(body[0]["included_item_source_kinds"].size() == 2);
    AISTUDIO_EXPECT(body[0]["included_item_source_kinds"][0] == "File");
}

AISTUDIO_TEST(ApiServer_ContextSnapshots_GetById_MissingId_Returns404) {
    RunningServer server(18151);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Get("/api/context/snapshots/does-not-exist");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
}

AISTUDIO_TEST(ApiServer_ContextSnapshots_GetById_ReturnsSnapshot) {
    RunningServer server(18152);

    ContextSnapshot snapshot;
    snapshot.id = "snap-2";
    snapshot.task_name = "another task";
    snapshot.included_item_ids = {"a.cpp"};
    server.context_snapshot_repository.Save(snapshot);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/context/snapshots/snap-2");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);
    AISTUDIO_EXPECT(Json::parse(res->body)["task_name"] == "another task");
}

AISTUDIO_TEST(ApiServer_ContextSnapshots_Restore_ResolvesFileBackedItems) {
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_context_restore_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "a.cpp", std::ios::binary);
        out << "int main() { return 0; }\n";
    }

    RunningServer server(18153, temp_dir.string());

    ContextSnapshot snapshot;
    snapshot.id = "snap-3";
    snapshot.included_item_ids = {"a.cpp"};
    server.context_snapshot_repository.Save(snapshot);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Post("/api/context/snapshots/snap-3/restore");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["id"] == "a.cpp");
    AISTUDIO_EXPECT(body[0]["content"] == "int main() { return 0; }\n");
}

AISTUDIO_TEST(ApiServer_ContextSnapshots_Restore_ResolvesSymbolBackedItems) {
    // docs/ROADMAP.md "Context Restore" -- end-to-end check that the
    // ApiServerIndexes' existing symbol_index actually reaches
    // ContextRestorer through the /restore handler, not just that
    // ContextRestorer itself can do it in isolation (see
    // test_context_restorer.cpp for the unit-level coverage).
    namespace fs = std::filesystem;
    const auto temp_dir = fs::temp_directory_path() / "aistudio_api_context_restore_symbol_test";
    fs::remove_all(temp_dir);
    fs::create_directories(temp_dir);
    {
        std::ofstream out(temp_dir / "Player.hpp", std::ios::binary);
        out << "class PlayerAttack {\n};\n";
    }

    RunningServer server(18155, temp_dir.string());
    AISTUDIO_EXPECT(server.symbol_index.Build(temp_dir.string()));

    ContextSnapshot snapshot;
    snapshot.id = "snap-symbol";
    snapshot.included_item_ids = {"Player.hpp:PlayerAttack"};
    snapshot.included_item_source_kinds = {ContextSourceKind::Symbol};
    server.context_snapshot_repository.Save(snapshot);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Post("/api/context/snapshots/snap-symbol/restore");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body.size() == 1);
    AISTUDIO_EXPECT(body[0]["id"] == "Player.hpp:PlayerAttack");
    AISTUDIO_EXPECT(body[0]["source"] == "Symbol");
}

AISTUDIO_TEST(ApiServer_ContextSnapshots_Restore_MissingId_Returns404) {
    RunningServer server(18154);
    httplib::Client client("127.0.0.1", server.port);

    const auto res = client.Post("/api/context/snapshots/does-not-exist/restore");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 404);
}

AISTUDIO_TEST(ApiServer_ContextUsage_ReflectsSavedAuditEntries) {
    RunningServer server(18124);

    ContextAuditEntry included;
    included.id = "audit-1";
    included.item_id = "a.cpp";
    included.action = ContextAuditAction::Included;
    included.detail = "tokens=100";
    included.timestamp = 1700000000;
    server.context_audit_repository.Save(included);

    ContextAuditEntry excluded;
    excluded.id = "audit-2";
    excluded.item_id = "b.cpp";
    excluded.action = ContextAuditAction::Excluded;
    excluded.detail = "tokens=9999";
    excluded.timestamp = 1700000001;
    server.context_audit_repository.Save(excluded);

    httplib::Client client("127.0.0.1", server.port);
    const auto res = client.Get("/api/context/usage");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["included"].size() == 1);
    AISTUDIO_EXPECT(body["included"][0]["item_id"] == "a.cpp");
    AISTUDIO_EXPECT(body["included"][0]["estimated_tokens"] == 100);
    AISTUDIO_EXPECT(body["included"][0]["priority"] == -1); // legacy detail, no "priority=" prefix
    AISTUDIO_EXPECT(body["excluded"].size() == 1);
    AISTUDIO_EXPECT(body["excluded"][0]["item_id"] == "b.cpp");
    AISTUDIO_EXPECT(body["total_included_tokens"] == 100);
}

namespace {

// Minimal duplicate of test_git_backend.cpp's TempGitRepo -- this file
// only needs enough of a repo to exercise AnyToJson's GitStatusResult/
// vector<GitCommitEntry> translation over a real HTTP round trip, not to
// re-verify GitBackend's own git plumbing (already covered there).
struct TempGitRepoForApiTest {
    std::filesystem::path root;

    TempGitRepoForApiTest() {
        static std::atomic<int> counter{0};
        root = std::filesystem::temp_directory_path() /
               std::filesystem::path("aistudio_api_git_test_" + std::to_string(counter++));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        (void)RunProcess("git", {"init"}, root.string());
        (void)RunProcess("git", {"config", "user.email", "test@example.com"}, root.string());
        (void)RunProcess("git", {"config", "user.name", "Test"}, root.string());
    }

    ~TempGitRepoForApiTest() { std::filesystem::remove_all(root); }

    void Commit(const std::string& file_content, const std::string& message) const {
        std::ofstream out(root / "f.txt", std::ios::binary);
        out << file_content;
        out.close();
        (void)RunProcess("git", {"add", "-A"}, root.string());
        (void)RunProcess("git", {"commit", "-m", message}, root.string());
    }

    [[nodiscard]] std::string RootString() const { return root.string(); }
};

} // namespace

AISTUDIO_TEST(ApiServer_GitStatusQuery_ReturnsStructuredJson) {
    TempGitRepoForApiTest repo;
    repo.Commit("1\n", "initial commit");
    {
        std::ofstream out(repo.root / "untracked.txt", std::ios::binary);
        out << "new\n";
    }

    RunningServer server(18160);
    auto git_backend = std::make_shared<GitBackend>();
    Config config;
    config.Set("backend.core.git.root", repo.RootString());
    git_backend->Configure(config);
    git_backend->Start();
    server.registry.Register(git_backend);

    httplib::Client client("127.0.0.1", server.port);
    const Json request_body{{"name", "git.status"}};
    const auto res = client.Post("/api/backends/core.git/queries", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["ok"] == true);
    bool has_untracked = false;
    for (const auto& entry : body["result"]["entries"]) {
        if (entry["path"] == "untracked.txt" && entry["status_code"] == "??") {
            has_untracked = true;
        }
    }
    AISTUDIO_EXPECT(has_untracked);
}

AISTUDIO_TEST(ApiServer_GitHistoryQuery_ReturnsStructuredJsonArray) {
    TempGitRepoForApiTest repo;
    repo.Commit("1\n", "first commit");
    repo.Commit("2\n", "second commit");

    RunningServer server(18161);
    auto git_backend = std::make_shared<GitBackend>();
    Config config;
    config.Set("backend.core.git.root", repo.RootString());
    git_backend->Configure(config);
    git_backend->Start();
    server.registry.Register(git_backend);

    httplib::Client client("127.0.0.1", server.port);
    const Json request_body{{"name", "git.history"}};
    const auto res = client.Post("/api/backends/core.git/queries", request_body.dump(), "application/json");
    AISTUDIO_EXPECT(res != nullptr);
    AISTUDIO_EXPECT(res->status == 200);

    const auto body = Json::parse(res->body);
    AISTUDIO_EXPECT(body["result"].is_array());
    AISTUDIO_EXPECT(body["result"].size() == 2);
    AISTUDIO_EXPECT(body["result"][0]["subject"] == "second commit");
    AISTUDIO_EXPECT(body["result"][1]["subject"] == "first commit");
}
