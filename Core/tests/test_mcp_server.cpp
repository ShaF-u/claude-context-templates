#include "test_framework.hpp"
#include "Core/Analysis/ImpactAnalyzer.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Context/ContextCache.hpp"
#include "Core/Context/ContextRetriever.hpp"
#include "Core/Context/ProjectRulesBackend.hpp"
#include "Core/Context/SentLedger.hpp"
#include "Core/Git/GitBackend.hpp"
#include "Core/Index/AstIndex.hpp"
#include "Core/Index/CallGraph.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/InheritanceGraph.hpp"
#include "Core/Index/ReferenceGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/MCP/McpServer.hpp"
#include "Core/Security/Sandbox.hpp"
#include "Core/Util/ProcessRunner.hpp"
#include "Core/Util/Utf8.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_mcp_server_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempProject() { fs::remove_all(root); }

    void WriteFile(const std::string& relative_path, const std::string& content) const {
        const fs::path full_path = root / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
    }

    [[nodiscard]] std::string RootString() const { return root.string(); }
};

// changed_impact_analysis needs a real git repo + real GitBackend to
// fetch a real `git diff` from -- same shape as test_git_backend.cpp's
// TempGitRepo, duplicated here per this codebase's per-file test-helper
// convention.
struct TempGitRepo {
    fs::path root;

    TempGitRepo() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_mcp_git_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
        RunGit({"init"});
        RunGit({"config", "user.email", "test@example.com"});
        RunGit({"config", "user.name", "Test"});
    }

    ~TempGitRepo() { fs::remove_all(root); }

    void WriteFile(const std::string& relative_path, const std::string& content) const {
        const fs::path full_path = root / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
    }

    void RunGit(const std::vector<std::string>& args) const { (void)RunProcess("git", args, root.string()); }

    void Commit(const std::string& message) const {
        RunGit({"add", "-A"});
        RunGit({"commit", "-m", message});
    }

    [[nodiscard]] std::string RootString() const { return root.string(); }
};

std::shared_ptr<GitBackend> MakeStartedGitBackend(const std::string& root) {
    auto backend = std::make_shared<GitBackend>();
    Config config;
    config.Set("project.root", root);
    backend->Configure(config);
    backend->Start();
    return backend;
}

std::shared_ptr<ProjectRulesBackend> MakeStartedProjectRulesBackend(const std::string& root) {
    auto backend = std::make_shared<ProjectRulesBackend>();
    Config config;
    config.Set("project.root", root);
    backend->Configure(config);
    backend->Start();
    return backend;
}

// Feeds each of `lines` (already-serialized JSON-RPC messages, one per
// line) through one McpServer::Run() call and returns every response
// line, parsed. Notifications correctly contribute nothing to this list.
std::vector<Json> RunLines(const McpServer& server, const std::vector<std::string>& lines) {
    std::ostringstream input;
    for (const auto& line : lines) {
        input << line << "\n";
    }
    std::istringstream in(input.str());
    std::ostringstream out;
    server.Run(in, out);

    std::vector<Json> responses;
    std::istringstream out_stream(out.str());
    std::string response_line;
    while (std::getline(out_stream, response_line)) {
        if (!response_line.empty()) {
            responses.push_back(Json::parse(response_line));
        }
    }
    return responses;
}

Json RunOne(const McpServer& server, const std::string& line) {
    const auto responses = RunLines(server, {line});
    AISTUDIO_EXPECT(responses.size() == 1);
    return responses.front();
}

Json ToolCallResult(const McpServer& server, const std::string& tool_name, const Json& arguments) {
    const Json request{
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", "tools/call"},
        {"params", Json{{"name", tool_name}, {"arguments", arguments}}},
    };
    return RunOne(server, request.dump());
}

// The "matches"/"items" list embedded (as a JSON string) in a
// successful tool call's content[0].text.
Json ParsedContent(const Json& response) {
    return Json::parse(response["result"]["content"][0]["text"].get<std::string>());
}

} // namespace

AISTUDIO_TEST(McpServer_Initialize_ReturnsProtocolVersionAndServerInfo) {
    const McpServer server(McpServerOptions{.server_name = "aistudio-core", .server_version = "0.1.0"});
    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})");

    AISTUDIO_EXPECT(response["id"] == 1);
    AISTUDIO_EXPECT(response["result"]["protocolVersion"].is_string());
    AISTUDIO_EXPECT(response["result"]["serverInfo"]["name"] == "aistudio-core");
    AISTUDIO_EXPECT(response["result"]["serverInfo"]["version"] == "0.1.0");
    AISTUDIO_EXPECT(response["result"].contains("capabilities"));
}

AISTUDIO_TEST(McpServer_Ping_ReturnsEmptyResult) {
    const McpServer server(McpServerOptions{});
    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":2,"method":"ping"})");
    AISTUDIO_EXPECT(response["id"] == 2);
    AISTUDIO_EXPECT(response["result"].is_object());
}

AISTUDIO_TEST(McpServer_Notification_ProducesNoResponse) {
    const McpServer server(McpServerOptions{});
    const auto responses =
        RunLines(server, {R"({"jsonrpc":"2.0","method":"notifications/initialized"})"});
    AISTUDIO_EXPECT(responses.empty());
}

AISTUDIO_TEST(McpServer_RequestWithoutId_ProducesNoResponse) {
    // Per JSON-RPC 2.0, absence of "id" makes any message a Notification,
    // not just ones under the "notifications/" method prefix.
    const McpServer server(McpServerOptions{});
    const auto responses = RunLines(server, {R"({"jsonrpc":"2.0","method":"tools/list"})"});
    AISTUDIO_EXPECT(responses.empty());
}

AISTUDIO_TEST(McpServer_MalformedJson_ReturnsParseError) {
    const McpServer server(McpServerOptions{});
    const auto response = RunOne(server, "not valid json {");
    AISTUDIO_EXPECT(response["error"]["code"] == -32700);
}

AISTUDIO_TEST(McpServer_UnknownMethod_ReturnsMethodNotFoundError) {
    const McpServer server(McpServerOptions{});
    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":3,"method":"foo/bar"})");
    AISTUDIO_EXPECT(response["error"]["code"] == -32601);
}

AISTUDIO_TEST(McpServer_ToolsList_EmptyOptions_ListsNoTools) {
    const McpServer server(McpServerOptions{});
    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})");
    AISTUDIO_EXPECT(response["result"]["tools"].empty());
}

AISTUDIO_TEST(McpServer_ToolsList_FullyConfigured_ListsAllFourTools) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const ContextRetriever retriever(ContextRetriever::Options{.symbol_index = &symbol_index});

    const McpServer server(McpServerOptions{
        .symbol_index = &symbol_index,
        .context_retriever = &retriever,
        .project_root = project.RootString(),
    });

    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    const auto& tools = response["result"]["tools"];
    // symbol_search / keyword_search / context_retrieve / context_fetch
    AISTUDIO_EXPECT(tools.size() == 4);
    for (const auto& tool : tools) {
        AISTUDIO_EXPECT(tool.contains("name"));
        AISTUDIO_EXPECT(tool.contains("description"));
        AISTUDIO_EXPECT(tool["inputSchema"]["type"] == "object");
    }
}

AISTUDIO_TEST(McpServer_ToolsList_AllGraphToolsConfigured_ListsAllTenTools) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const ContextRetriever retriever(ContextRetriever::Options{.symbol_index = &symbol_index});
    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    InheritanceGraph inheritance_graph;
    inheritance_graph.Build(project.RootString());
    ReferenceGraph reference_graph;
    reference_graph.Build(project.RootString());
    AstIndex ast_index;
    ast_index.Build(project.RootString());
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);

    const McpServer server(McpServerOptions{
        .symbol_index = &symbol_index,
        .context_retriever = &retriever,
        .include_graph = &include_graph,
        .call_graph = &call_graph,
        .inheritance_graph = &inheritance_graph,
        .reference_graph = &reference_graph,
        .ast_index = &ast_index,
        .impact_analyzer = &impact_analyzer,
        .project_root = project.RootString(),
    });

    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    AISTUDIO_EXPECT(response["result"]["tools"].size() == 10);
}

AISTUDIO_TEST(McpServer_ToolsCall_SymbolSearch_ReturnsMatches) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const McpServer server(McpServerOptions{.symbol_index = &symbol_index});

    const auto response = ToolCallResult(server, "symbol_search", Json{{"query", "PlayerAttack"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["matches"].size() == 1);
    AISTUDIO_EXPECT(content["matches"][0]["name"] == "PlayerAttack");
}

AISTUDIO_TEST(McpServer_ToolsCall_SymbolSearch_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "symbol_search", Json{{"query", "Foo"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_SymbolSearch_MissingQuery_IsError) {
    TempProject project;
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const McpServer server(McpServerOptions{.symbol_index = &symbol_index});

    const auto response = ToolCallResult(server, "symbol_search", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_SymbolSearch_FirewallBlocksMatch) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns.
    project.WriteFile("my_secret.hpp", "class Attack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.symbol_index = &symbol_index, .firewall = &firewall});

    const auto response = ToolCallResult(server, "symbol_search", Json{{"query", "Attack"}});
    AISTUDIO_EXPECT(ParsedContent(response)["matches"].empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_KeywordSearch_ReturnsMatches) {
    TempProject project;
    project.WriteFile("a.cpp", "// implements combat strategy logic\nvoid Foo() {}\n");

    const McpServer server(McpServerOptions{.project_root = project.RootString()});
    const auto response = ToolCallResult(server, "keyword_search", Json{{"query", "strategy"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    AISTUDIO_EXPECT(ParsedContent(response)["matches"].size() == 1);
}

AISTUDIO_TEST(McpServer_ToolsCall_KeywordSearch_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "keyword_search", Json{{"query", "TODO"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_KeywordSearch_FirewallBlocksMatch) {
    TempProject project;
    project.WriteFile(".env", "// strategy notes for combat balancing\n");

    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.project_root = project.RootString(), .firewall = &firewall});

    const auto response = ToolCallResult(server, "keyword_search", Json{{"query", "strategy"}});
    AISTUDIO_EXPECT(ParsedContent(response)["matches"].empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_ContextRetrieve_ReturnsItems) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const ContextRetriever retriever(ContextRetriever::Options{.symbol_index = &symbol_index});
    const McpServer server(McpServerOptions{.context_retriever = &retriever});

    const auto response = ToolCallResult(server, "context_retrieve", Json{{"intent", "PlayerAttack"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    AISTUDIO_EXPECT(!ParsedContent(response)["items"].empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_ContextRetrieve_WithContextCache_SecondCallIsCacheHit) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const ContextRetriever retriever(ContextRetriever::Options{.symbol_index = &symbol_index});
    ContextCache context_cache;
    const McpServer server(McpServerOptions{.context_retriever = &retriever, .context_cache = &context_cache});

    const auto first = ToolCallResult(server, "context_retrieve", Json{{"intent", "PlayerAttack"}});
    const auto second = ToolCallResult(server, "context_retrieve", Json{{"intent", "PlayerAttack"}});

    AISTUDIO_EXPECT(first["result"].value("isError", false) == false);
    AISTUDIO_EXPECT(ParsedContent(first)["items"] == ParsedContent(second)["items"]);
    // Proves the second tools/call actually went through ContextCache
    // (a hit) rather than McpServer silently ignoring the field and
    // calling context_retriever->Retrieve() directly both times.
    AISTUDIO_EXPECT(context_cache.Stats().hits == 1);
    AISTUDIO_EXPECT(context_cache.Stats().misses == 1);
}

AISTUDIO_TEST(McpServer_ToolsCall_ContextRetrieve_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_UnknownTool_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "does_not_exist", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_IncludeGraph_Includes_ReturnsIncludedFiles) {
    TempProject project;
    project.WriteFile("a.hpp", "void Foo();\n");
    project.WriteFile("b.hpp", "#include \"a.hpp\"\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    const McpServer server(McpServerOptions{.include_graph = &include_graph});

    const auto response = ToolCallResult(server, "include_graph", Json{{"file_path", "b.hpp"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["direction"] == "includes");
    AISTUDIO_EXPECT(content["files"].size() == 1);
    AISTUDIO_EXPECT(content["files"][0] == "a.hpp");
}

AISTUDIO_TEST(McpServer_ToolsCall_IncludeGraph_IncludedBy_ReturnsIncludingFiles) {
    TempProject project;
    project.WriteFile("a.hpp", "void Foo();\n");
    project.WriteFile("b.hpp", "#include \"a.hpp\"\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    const McpServer server(McpServerOptions{.include_graph = &include_graph});

    const auto response =
        ToolCallResult(server, "include_graph", Json{{"file_path", "a.hpp"}, {"direction", "included_by"}});
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["files"].size() == 1);
    AISTUDIO_EXPECT(content["files"][0] == "b.hpp");
}

AISTUDIO_TEST(McpServer_ToolsCall_IncludeGraph_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "include_graph", Json{{"file_path", "a.hpp"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_IncludeGraph_FirewallBlocksMatch) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "void Foo();\n");
    project.WriteFile("b.hpp", "#include \"my_secret.hpp\"\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.include_graph = &include_graph, .firewall = &firewall});

    const auto response = ToolCallResult(server, "include_graph", Json{{"file_path", "b.hpp"}});
    AISTUDIO_EXPECT(ParsedContent(response)["files"].empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_CallGraph_Callers_ReturnsCallSites) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");
    project.WriteFile("b.cpp", "void Caller() {\n    Foo();\n}\n");

    CallGraph call_graph;
    call_graph.Build(project.RootString());
    const McpServer server(McpServerOptions{.call_graph = &call_graph});

    const auto response = ToolCallResult(server, "call_graph", Json{{"symbol_name", "Foo"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["direction"] == "callers");
    AISTUDIO_EXPECT(content["edges"].size() == 1);
    AISTUDIO_EXPECT(content["edges"][0]["caller_name"] == "Caller");
    AISTUDIO_EXPECT(content["edges"][0]["caller_file"] == "b.cpp");
}

AISTUDIO_TEST(McpServer_ToolsCall_CallGraph_Callees_ReturnsCallsMadeInsideSymbol) {
    TempProject project;
    project.WriteFile("a.cpp", "void Caller() {\n    Foo();\n}\n");

    CallGraph call_graph;
    call_graph.Build(project.RootString());
    const McpServer server(McpServerOptions{.call_graph = &call_graph});

    const auto response =
        ToolCallResult(server, "call_graph", Json{{"symbol_name", "Caller"}, {"direction", "callees"}});
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["edges"].size() == 1);
    AISTUDIO_EXPECT(content["edges"][0]["callee_text"] == "Foo");
}

AISTUDIO_TEST(McpServer_ToolsCall_CallGraph_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "call_graph", Json{{"symbol_name", "Foo"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_CallGraph_FirewallBlocksMatch) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");
    project.WriteFile("my_secret.cpp", "void Caller() {\n    Foo();\n}\n");

    CallGraph call_graph;
    call_graph.Build(project.RootString());
    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.call_graph = &call_graph, .firewall = &firewall});

    const auto response = ToolCallResult(server, "call_graph", Json{{"symbol_name", "Foo"}});
    AISTUDIO_EXPECT(ParsedContent(response)["edges"].empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_InheritanceGraph_Derived_ReturnsSubclasses) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph inheritance_graph;
    inheritance_graph.Build(project.RootString());
    const McpServer server(McpServerOptions{.inheritance_graph = &inheritance_graph});

    const auto response = ToolCallResult(server, "inheritance_graph", Json{{"class_name", "Bar"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["direction"] == "derived");
    AISTUDIO_EXPECT(content["edges"].size() == 1);
    AISTUDIO_EXPECT(content["edges"][0]["derived_name"] == "Foo");
}

AISTUDIO_TEST(McpServer_ToolsCall_InheritanceGraph_Bases_ReturnsBaseClasses) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph inheritance_graph;
    inheritance_graph.Build(project.RootString());
    const McpServer server(McpServerOptions{.inheritance_graph = &inheritance_graph});

    const auto response =
        ToolCallResult(server, "inheritance_graph", Json{{"class_name", "Foo"}, {"direction", "bases"}});
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["edges"].size() == 1);
    AISTUDIO_EXPECT(content["edges"][0]["base_name"] == "Bar");
}

AISTUDIO_TEST(McpServer_ToolsCall_InheritanceGraph_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "inheritance_graph", Json{{"class_name", "Foo"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_InheritanceGraph_FirewallBlocksMatch) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph inheritance_graph;
    inheritance_graph.Build(project.RootString());
    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.inheritance_graph = &inheritance_graph, .firewall = &firewall});

    const auto response = ToolCallResult(server, "inheritance_graph", Json{{"class_name", "Bar"}});
    AISTUDIO_EXPECT(ParsedContent(response)["edges"].empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_ReferenceGraph_ReturnsReferencingLocations) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph reference_graph;
    reference_graph.Build(project.RootString());
    const McpServer server(McpServerOptions{.reference_graph = &reference_graph});

    const auto response = ToolCallResult(server, "reference_graph", Json{{"type_name", "Bar"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["references"].size() == 1);
    AISTUDIO_EXPECT(content["references"][0]["referencing_file"] == "a.hpp");
}

AISTUDIO_TEST(McpServer_ToolsCall_ReferenceGraph_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "reference_graph", Json{{"type_name", "Bar"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_ReferenceGraph_FirewallBlocksMatch) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph reference_graph;
    reference_graph.Build(project.RootString());
    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.reference_graph = &reference_graph, .firewall = &firewall});

    const auto response = ToolCallResult(server, "reference_graph", Json{{"type_name", "Bar"}});
    AISTUDIO_EXPECT(ParsedContent(response)["references"].empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_AstTree_ReturnsTree) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");

    AstIndex ast_index;
    ast_index.Build(project.RootString());
    const McpServer server(McpServerOptions{.ast_index = &ast_index});

    const auto response = ToolCallResult(server, "ast_tree", Json{{"file_path", "a.hpp"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["file_path"] == "a.hpp");
    AISTUDIO_EXPECT(content["tree"].contains("kind"));
    AISTUDIO_EXPECT(content["tree"].contains("children"));
}

AISTUDIO_TEST(McpServer_ToolsCall_AstTree_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "ast_tree", Json{{"file_path", "a.hpp"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_AstTree_MissingFile_IsError) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");

    AstIndex ast_index;
    ast_index.Build(project.RootString());
    const McpServer server(McpServerOptions{.ast_index = &ast_index});

    const auto response = ToolCallResult(server, "ast_tree", Json{{"file_path", "not_indexed.hpp"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_AstTree_FirewallBlocksMatch) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "class Foo {};\n");

    AstIndex ast_index;
    ast_index.Build(project.RootString());
    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.ast_index = &ast_index, .firewall = &firewall});

    const auto response = ToolCallResult(server, "ast_tree", Json{{"file_path", "my_secret.hpp"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_ImpactAnalysis_ReturnsAffectedFilesAndSymbols) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");
    project.WriteFile("b.cpp", "void Caller() {\n    Foo();\n}\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);
    const McpServer server(McpServerOptions{.impact_analyzer = &impact_analyzer});

    const auto response = ToolCallResult(server, "impact_analysis", Json{{"file_path", "a.cpp"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);
    AISTUDIO_EXPECT(content["target_file"] == "a.cpp");
    AISTUDIO_EXPECT(content["affected_symbols"].size() == 1);
    AISTUDIO_EXPECT(content["affected_symbols"][0]["symbol_name"] == "Foo");
    AISTUDIO_EXPECT(content["affected_symbols"][0]["callers"].size() == 1);
    AISTUDIO_EXPECT(content["affected_symbols"][0]["callers"][0]["caller_name"] == "Caller");
}

AISTUDIO_TEST(McpServer_ToolsCall_ImpactAnalysis_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "impact_analysis", Json{{"file_path", "a.cpp"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_ImpactAnalysis_FirewallBlocksAffectedFilesAndCallers) {
    TempProject project;
    project.WriteFile("a.hpp", "void Foo();\n");
    project.WriteFile("my_secret.hpp", "#include \"a.hpp\"\n");
    project.WriteFile("a.cpp", "void Foo() {\n}\n");
    project.WriteFile("my_secret.cpp", "void Caller() {\n    Foo();\n}\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);
    const Sandbox firewall(project.RootString());
    const McpServer server(McpServerOptions{.impact_analyzer = &impact_analyzer, .firewall = &firewall});

    {
        const auto response = ToolCallResult(server, "impact_analysis", Json{{"file_path", "a.hpp"}});
        AISTUDIO_EXPECT(ParsedContent(response)["affected_files"].empty());
    }
    {
        const auto response = ToolCallResult(server, "impact_analysis", Json{{"file_path", "a.cpp"}});
        AISTUDIO_EXPECT(ParsedContent(response)["affected_symbols"][0]["callers"].empty());
    }
}

AISTUDIO_TEST(McpServer_ToolsList_ImpactAnalyzerWithoutBackendRegistry_OmitsChangedImpactAnalysis) {
    TempProject project;
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);
    // backend_registry deliberately left null.
    const McpServer server(McpServerOptions{.impact_analyzer = &impact_analyzer});

    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    for (const auto& tool : response["result"]["tools"]) {
        AISTUDIO_EXPECT(tool["name"] != "changed_impact_analysis");
    }
}

AISTUDIO_TEST(McpServer_ToolsCall_ChangedImpactAnalysis_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "changed_impact_analysis", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_ChangedImpactAnalysis_ReturnsChangedSymbolAndCallers) {
    TempGitRepo repo;
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 1;\n}\n");
    repo.WriteFile("b.cpp", "void Caller() {\n    Foo();\n}\n");
    repo.Commit("initial");
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 999;\n}\n");

    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());
    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));

    const McpServer server(McpServerOptions{.impact_analyzer = &impact_analyzer, .backend_registry = &registry});

    {
        const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
        bool has_tool = false;
        for (const auto& tool : response["result"]["tools"]) {
            if (tool["name"] == "changed_impact_analysis") {
                has_tool = true;
                AISTUDIO_EXPECT(!tool["inputSchema"].contains("required"));
            }
        }
        AISTUDIO_EXPECT(has_tool);
    }

    const auto response = ToolCallResult(server, "changed_impact_analysis", Json::object());
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(!content["changed_files"].empty());
    bool found_foo = false;
    for (const auto& symbol : content["changed_symbols"]) {
        if (symbol["symbol_name"] == "Foo") {
            found_foo = true;
            AISTUDIO_EXPECT(symbol["kind"] == "Function");
            AISTUDIO_EXPECT(symbol["callers"].size() == 1);
            AISTUDIO_EXPECT(symbol["callers"][0]["caller_name"] == "Caller");
        }
    }
    AISTUDIO_EXPECT(found_foo);
}

AISTUDIO_TEST(McpServer_ToolsCall_ChangedImpactAnalysis_DetectsStagedOnlyChange) {
    TempGitRepo repo;
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 1;\n}\n");
    repo.Commit("initial");
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 999;\n}\n");
    repo.RunGit({"add", "a.cpp"}); // staged, not committed

    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());
    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));

    const McpServer server(McpServerOptions{.impact_analyzer = &impact_analyzer, .backend_registry = &registry});

    const auto response = ToolCallResult(server, "changed_impact_analysis", Json::object());
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    bool found_foo = false;
    for (const auto& symbol : content["changed_symbols"]) {
        if (symbol["symbol_name"] == "Foo") {
            found_foo = true;
        }
    }
    AISTUDIO_EXPECT(found_foo);
}

AISTUDIO_TEST(McpServer_ToolsCall_ProjectRules_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "project_rules", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_ProjectRules_ReturnsCombinedFileContent) {
    TempProject project;
    project.WriteFile("CLAUDE.md", "no raw pointers\n");
    project.WriteFile("AGENT.md", "small commits\n");

    BackendRegistry registry;
    registry.Register(MakeStartedProjectRulesBackend(project.RootString()));
    const McpServer server(McpServerOptions{.backend_registry = &registry});

    {
        const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
        bool has_tool = false;
        for (const auto& tool : response["result"]["tools"]) {
            if (tool["name"] == "project_rules") has_tool = true;
        }
        AISTUDIO_EXPECT(has_tool);
    }

    const auto response = ToolCallResult(server, "project_rules", Json::object());
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto text = response["result"]["content"][0]["text"].get<std::string>();
    AISTUDIO_EXPECT(text.find("no raw pointers") != std::string::npos);
    AISTUDIO_EXPECT(text.find("small commits") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ToolsCall_SimilarChangeSearch_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "similar_change_search", Json{{"symbol_name", "Foo"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_SimilarChangeSearch_MissingSymbolName_IsError) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(McpServerOptions{.backend_registry = &registry});

    const auto response = ToolCallResult(server, "similar_change_search", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_SimilarChangeSearch_ReturnsPastCommitsWithDiffs) {
    TempGitRepo repo;
    repo.WriteFile("f.cpp", "void Foo() {\n}\n");
    repo.Commit("add Foo");
    repo.WriteFile("f.cpp", "void Foo() {\n}\nvoid Bar() {\n}\n");
    repo.Commit("add Bar");

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(McpServerOptions{.backend_registry = &registry});

    {
        const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
        bool has_tool = false;
        for (const auto& tool : response["result"]["tools"]) {
            if (tool["name"] == "similar_change_search") has_tool = true;
        }
        AISTUDIO_EXPECT(has_tool);
    }

    const auto response = ToolCallResult(server, "similar_change_search", Json{{"symbol_name", "Foo"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(content["symbol_name"] == "Foo");
    AISTUDIO_EXPECT(content["commits"].size() == 1);
    AISTUDIO_EXPECT(content["commits"][0]["subject"] == "add Foo");
    AISTUDIO_EXPECT(content["commits"][0]["diff"].get<std::string>().find("+void Foo()") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ToolsCall_BranchImpactAnalysis_NotConfigured_IsError) {
    const McpServer server(McpServerOptions{});
    const auto response = ToolCallResult(server, "branch_impact_analysis", Json{{"base_branch", "base"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_BranchImpactAnalysis_MissingBaseBranch_IsError) {
    TempGitRepo repo;
    SymbolIndex symbol_index;
    IncludeGraph include_graph;
    CallGraph call_graph;
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(McpServerOptions{.impact_analyzer = &impact_analyzer, .backend_registry = &registry});

    const auto response = ToolCallResult(server, "branch_impact_analysis", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_BranchImpactAnalysis_ReturnsChangesSinceBaseIncludingCommitted) {
    TempGitRepo repo;
    repo.RunGit({"checkout", "-b", "base"});
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 1;\n}\n");
    repo.WriteFile("b.cpp", "void Caller() {\n    Foo();\n}\n");
    repo.Commit("base commit");
    repo.RunGit({"branch", "feature"});
    repo.RunGit({"checkout", "feature"});
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 999;\n}\n");
    repo.Commit("feature commit"); // already committed, not just working-tree

    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());
    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    const ImpactAnalyzer impact_analyzer(include_graph, call_graph, symbol_index);

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));

    const McpServer server(McpServerOptions{.impact_analyzer = &impact_analyzer, .backend_registry = &registry});

    {
        const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
        bool has_tool = false;
        for (const auto& tool : response["result"]["tools"]) {
            if (tool["name"] == "branch_impact_analysis") has_tool = true;
        }
        AISTUDIO_EXPECT(has_tool);
    }

    const auto response = ToolCallResult(server, "branch_impact_analysis", Json{{"base_branch", "base"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(content["base_branch"] == "base");
    bool found_foo = false;
    for (const auto& symbol : content["changed_symbols"]) {
        if (symbol["symbol_name"] == "Foo") {
            found_foo = true;
            AISTUDIO_EXPECT(symbol["callers"].size() == 1);
            AISTUDIO_EXPECT(symbol["callers"][0]["caller_name"] == "Caller");
        }
    }
    AISTUDIO_EXPECT(found_foo);
}

AISTUDIO_TEST(McpServer_ToolsList_GitWriteCommandsDefaultOff_OmitsGitWriteTools) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    // enable_git_write_commands deliberately left at its default (false).
    const McpServer server(McpServerOptions{.backend_registry = &registry});

    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    for (const auto& tool : response["result"]["tools"]) {
        AISTUDIO_EXPECT(tool["name"] != "git_commit");
        AISTUDIO_EXPECT(tool["name"] != "git_branch");
        AISTUDIO_EXPECT(tool["name"] != "git_stash");
        AISTUDIO_EXPECT(tool["name"] != "git_checkout");
        AISTUDIO_EXPECT(tool["name"] != "git_tag");
    }
}

AISTUDIO_TEST(McpServer_ToolsList_GitWriteCommandsEnabled_ListsAllFiveGitWriteTools) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":5,"method":"tools/list"})");
    bool has_commit = false, has_branch = false, has_stash = false, has_checkout = false, has_tag = false;
    for (const auto& tool : response["result"]["tools"]) {
        if (tool["name"] == "git_commit") has_commit = true;
        if (tool["name"] == "git_branch") has_branch = true;
        if (tool["name"] == "git_stash") has_stash = true;
        if (tool["name"] == "git_checkout") has_checkout = true;
        if (tool["name"] == "git_tag") has_tag = true;
    }
    AISTUDIO_EXPECT(has_commit);
    AISTUDIO_EXPECT(has_branch);
    AISTUDIO_EXPECT(has_stash);
    AISTUDIO_EXPECT(has_checkout);
    AISTUDIO_EXPECT(has_tag);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitCommit_Disabled_IsError) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(McpServerOptions{.backend_registry = &registry}); // toggle left off

    const auto response = ToolCallResult(server, "git_commit", Json{{"message", "wip"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitCommit_Enabled_ActuallyCommits) {
    TempGitRepo repo;
    repo.WriteFile("a.txt", "hello\n");
    repo.Commit("initial");
    repo.WriteFile("a.txt", "hello\nmodified\n");

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_commit", Json{{"message", "via mcp"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);

    const auto log_result = RunProcess("git", {"log", "--pretty=format:%s"}, repo.RootString());
    AISTUDIO_EXPECT(log_result.IsOk());
    AISTUDIO_EXPECT(log_result.Value().output.find("via mcp") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitCommit_MissingMessage_IsError) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_commit", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitBranch_Enabled_ActuallyCreatesBranch) {
    TempGitRepo repo;
    repo.WriteFile("a.txt", "hello\n");
    repo.Commit("initial");

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_branch", Json{{"name", "feature/via-mcp"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);

    const auto list_result = RunProcess("git", {"branch", "--list"}, repo.RootString());
    AISTUDIO_EXPECT(list_result.IsOk());
    AISTUDIO_EXPECT(list_result.Value().output.find("feature/via-mcp") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitStash_Enabled_ActuallyStashesChanges) {
    TempGitRepo repo;
    repo.WriteFile("a.txt", "hello\n");
    repo.Commit("initial");
    repo.WriteFile("a.txt", "hello\nmodified\n");

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_stash", Json::object());
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);

    const auto status_result = RunProcess("git", {"status", "--porcelain"}, repo.RootString());
    AISTUDIO_EXPECT(status_result.IsOk());
    AISTUDIO_EXPECT(status_result.Value().output.empty());
}

AISTUDIO_TEST(McpServer_ToolsCall_GitCheckout_Disabled_IsError) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(McpServerOptions{.backend_registry = &registry}); // toggle left off

    const auto response = ToolCallResult(server, "git_checkout", Json{{"branch", "feature"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitCheckout_MissingBranch_IsError) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_checkout", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitCheckout_Enabled_ActuallySwitchesBranch) {
    TempGitRepo repo;
    repo.RunGit({"checkout", "-b", "base"});
    repo.WriteFile("a.txt", "hello\n");
    repo.Commit("initial");
    repo.RunGit({"branch", "feature"});

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_checkout", Json{{"branch", "feature"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);

    const auto branch_result = RunProcess("git", {"branch", "--show-current"}, repo.RootString());
    AISTUDIO_EXPECT(branch_result.IsOk());
    AISTUDIO_EXPECT(branch_result.Value().output.find("feature") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitTag_Disabled_IsError) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(McpServerOptions{.backend_registry = &registry}); // toggle left off

    const auto response = ToolCallResult(server, "git_tag", Json{{"name", "v1.0.0"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitTag_MissingName_IsError) {
    TempGitRepo repo;
    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_tag", Json::object());
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ToolsCall_GitTag_Enabled_ActuallyCreatesTag) {
    TempGitRepo repo;
    repo.WriteFile("a.txt", "hello\n");
    repo.Commit("initial");

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(
        McpServerOptions{.backend_registry = &registry, .enable_git_write_commands = true});

    const auto response = ToolCallResult(server, "git_tag", Json{{"name", "v1.0.0"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);

    const auto list_result = RunProcess("git", {"tag", "--list"}, repo.RootString());
    AISTUDIO_EXPECT(list_result.IsOk());
    AISTUDIO_EXPECT(list_result.Value().output.find("v1.0.0") != std::string::npos);
}

// --- context_retrieve response budget + context_fetch (MASTER_SPEC #99)

namespace {

// Fixed oversized items, so budget tests don't depend on this
// repository's own file sizes.
class BulkContextBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "test.bulk_context"; }
    [[nodiscard]] std::string Name() const override { return "Bulk Context"; }
    [[nodiscard]] std::string Version() const override { return "1.0.0"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override { return {"context.provide.test@1.0.0"}; }
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }
    CommandResult Dispatch(const Command&) override {
        return CommandResult::Fail(Error{.code = ErrorCode::NotFound, .message = "no commands"});
    }
    QueryResult Handle(const Query&) override {
        return QueryResult::Fail(Error{.code = ErrorCode::NotFound, .message = "no queries"});
    }

    [[nodiscard]] std::vector<ContextItem> ProvideContext(const std::string&) const override {
        std::vector<ContextItem> items;
        for (int i = 0; i < 5; ++i) {
            ContextItem item;
            item.id = "bulk/file" + std::to_string(i) + ".txt";
            item.source = ContextSourceKind::File;
            item.priority = 60 - i * 10; // 60, 50, 40, 30, 20
            item.content = std::string(4000, 'x');
            item.estimated_tokens = EstimateTokens(item.content);
            items.push_back(std::move(item));
        }
        return items;
    }
};

} // namespace

AISTUDIO_TEST(McpServer_ContextRetrieve_NoBudget_ReturnsEveryItemInFullAndNoBudgetBlock) {
    BackendRegistry registry;
    registry.Register(std::make_shared<BulkContextBackend>());
    const ContextRetriever retriever(ContextRetriever::Options{.backend_registry = &registry});
    const McpServer server(McpServerOptions{.context_retriever = &retriever});

    const auto content = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));

    // Unchanged pre-existing shape.
    AISTUDIO_EXPECT(content["items"].size() == 5);
    AISTUDIO_EXPECT(content["items"][0]["content"].get<std::string>().size() == 4000);
    AISTUDIO_EXPECT(!content.contains("omitted"));
    AISTUDIO_EXPECT(!content.contains("budget"));
}

AISTUDIO_TEST(McpServer_ContextRetrieve_Budget_KeepsHighestPriorityAndStubsTheRest) {
    BackendRegistry registry;
    registry.Register(std::make_shared<BulkContextBackend>());
    const ContextRetriever retriever(ContextRetriever::Options{.backend_registry = &registry});
    const McpServer server(McpServerOptions{
        .context_retriever = &retriever,
        .context_response_budget_tokens = 1200, // room for roughly one 1000-token item
    });

    const auto content = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));

    AISTUDIO_EXPECT(!content["items"].empty());
    AISTUDIO_EXPECT(!content["omitted"].empty());
    AISTUDIO_EXPECT(content["items"].size() + content["omitted"].size() == 5);
    AISTUDIO_EXPECT(content["items"][0]["id"] == "bulk/file0.txt");
    // No content, but enough to decide whether to fetch it.
    AISTUDIO_EXPECT(!content["omitted"][0].contains("content"));
    AISTUDIO_EXPECT(content["omitted"][0].contains("id"));
    AISTUDIO_EXPECT(content["omitted"][0]["reason"] == "budget");
    AISTUDIO_EXPECT(content["budget"]["max_tokens"] == 1200);
    AISTUDIO_EXPECT(content["budget"]["unit"] == "estimate");
}

AISTUDIO_TEST(McpServer_ContextRetrieve_Budget_ChargesTheSerializedEntryNotJustContent) {
    BackendRegistry registry;
    registry.Register(std::make_shared<BulkContextBackend>());
    const ContextRetriever retriever(ContextRetriever::Options{.backend_registry = &registry});
    const std::int64_t budget = 2500;
    const McpServer server(McpServerOptions{
        .context_retriever = &retriever,
        .context_response_budget_tokens = budget,
    });

    const auto content = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));

    // What's reported as used covers what is actually sent.
    std::int64_t serialized = 0;
    for (const auto& item : content["items"]) {
        serialized += EstimateTokens(item.dump());
    }
    AISTUDIO_EXPECT(serialized <= budget);
    AISTUDIO_EXPECT(content["budget"]["used_tokens"].get<std::int64_t>() <= budget);
    AISTUDIO_EXPECT(content["budget"]["used_tokens"].get<std::int64_t>() >= serialized);
}

AISTUDIO_TEST(McpServer_ContextRetrieve_Budget_CompressesRatherThanDroppingWhenItCan) {
    BackendRegistry registry;
    registry.Register(std::make_shared<BulkContextBackend>());
    const ContextRetriever retriever(ContextRetriever::Options{.backend_registry = &registry});
    const McpServer server(McpServerOptions{
        .context_retriever = &retriever,
        .context_response_budget_tokens = 1600,
    });

    const auto content = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));

    // Squeezed in as a Summary rather than excluded outright.
    bool has_summary = false;
    for (const auto& item : content["items"]) {
        if (item["compression"] == "Summary") {
            has_summary = true;
            AISTUDIO_EXPECT(item["content"].get<std::string>().find("omitted") != std::string::npos);
        }
    }
    AISTUDIO_EXPECT(has_summary);
}

AISTUDIO_TEST(McpServer_ContextRetrieve_SentLedgerDisabled_ReturnsFullContentEveryCall) {
    BackendRegistry registry;
    registry.Register(std::make_shared<BulkContextBackend>());
    const ContextRetriever retriever(ContextRetriever::Options{.backend_registry = &registry});
    SentLedger ledger;
    // suppress_resent_content left false (default): sent_ledger being set
    // must have no effect, byte-for-byte the pre-existing response.
    const McpServer server(McpServerOptions{.context_retriever = &retriever, .sent_ledger = &ledger});

    const auto first = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));
    const auto second = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));

    AISTUDIO_EXPECT(first["items"][0]["content"].get<std::string>().size() == 4000);
    AISTUDIO_EXPECT(second["items"][0]["content"].get<std::string>().size() == 4000);
    AISTUDIO_EXPECT(!second["items"][0].contains("unchanged_since"));
}

AISTUDIO_TEST(McpServer_ContextRetrieve_SentLedgerEnabled_SecondCallStubsUnchangedItems) {
    BackendRegistry registry;
    registry.Register(std::make_shared<BulkContextBackend>());
    const ContextRetriever retriever(ContextRetriever::Options{.backend_registry = &registry});
    SentLedger ledger;
    const McpServer server(
        McpServerOptions{.context_retriever = &retriever, .suppress_resent_content = true, .sent_ledger = &ledger});

    const auto first = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));
    AISTUDIO_EXPECT(first["items"][0].contains("content"));
    AISTUDIO_EXPECT(!first["items"][0].contains("unchanged_since"));

    const auto second = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));
    AISTUDIO_EXPECT(!second["items"][0].contains("content"));
    AISTUDIO_EXPECT(second["items"][0]["unchanged_since"] == 1);
    AISTUDIO_EXPECT(second["items"][0]["id"] == first["items"][0]["id"]);
}

AISTUDIO_TEST(McpServer_ContextRetrieve_SentLedgerEnabled_NoLedgerConfigured_HasNoEffect) {
    BackendRegistry registry;
    registry.Register(std::make_shared<BulkContextBackend>());
    const ContextRetriever retriever(ContextRetriever::Options{.backend_registry = &registry});
    // suppress_resent_content true but sent_ledger left nullptr -- must
    // not crash, and must behave exactly as if it were false.
    const McpServer server(McpServerOptions{.context_retriever = &retriever, .suppress_resent_content = true});

    const auto first = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));
    const auto second = ParsedContent(ToolCallResult(server, "context_retrieve", Json{{"intent", "anything"}}));

    AISTUDIO_EXPECT(second["items"][0].contains("content"));
    AISTUDIO_EXPECT(!second["items"][0].contains("unchanged_since"));
}

// docs/ROADMAP.md CE-5: context_fetch's `source` argument lets omitted
// Symbol/Dependency/GitDiff/Custom items be recovered, mirroring the
// `source` field the caller already received on the omitted item rather
// than guessing a kind from the id's own shape.

AISTUDIO_TEST(McpServer_ContextFetch_SourceSymbol_ReturnsContextAroundDeclaration) {
    TempProject project;
    project.WriteFile("Player.hpp", "// line1\n// line2\nclass PlayerAttack {\n};\n// line5\n");
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const McpServer server(
        McpServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const auto response = ToolCallResult(
        server, "context_fetch", Json{{"id", "Player.hpp:PlayerAttack"}, {"source", "Symbol"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(content["source"] == "Symbol");
    AISTUDIO_EXPECT(content["resolved_file"] == "Player.hpp");
    AISTUDIO_EXPECT(content["symbol_line"] == 3);
    AISTUDIO_EXPECT(content["content"].get<std::string>().find("class PlayerAttack") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceSymbol_UnknownSymbol_IsError) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    const McpServer server(
        McpServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const auto response = ToolCallResult(
        server, "context_fetch", Json{{"id", "Player.hpp:DoesNotExist"}, {"source", "Symbol"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceSymbol_NotConfigured_IsError) {
    TempProject project;
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response = ToolCallResult(
        server, "context_fetch", Json{{"id", "Player.hpp:PlayerAttack"}, {"source", "Symbol"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceDependency_ReturnsSynthesizedRelationship) {
    TempProject project;
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response = ToolCallResult(
        server, "context_fetch", Json{{"id", "A.hpp->B.hpp"}, {"source", "Dependency"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(content["source"] == "Dependency");
    AISTUDIO_EXPECT(content["content"] == "A.hpp includes B.hpp");
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceDependency_MalformedId_IsError) {
    TempProject project;
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response =
        ToolCallResult(server, "context_fetch", Json{{"id", "not_an_edge"}, {"source", "Dependency"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceGitDiff_ReturnsCommitDiff) {
    TempGitRepo repo;
    repo.WriteFile("f.cpp", "void Foo() {\n}\n");
    repo.Commit("add Foo");
    const auto sha_result = RunProcess("git", {"rev-parse", "HEAD"}, repo.RootString());
    AISTUDIO_EXPECT(sha_result);
    std::string sha = sha_result.Value().output;
    while (!sha.empty() && (sha.back() == '\n' || sha.back() == '\r')) sha.pop_back();

    BackendRegistry registry;
    registry.Register(MakeStartedGitBackend(repo.RootString()));
    const McpServer server(McpServerOptions{.backend_registry = &registry, .project_root = repo.RootString()});

    const auto response = ToolCallResult(
        server, "context_fetch", Json{{"id", "git/commit/" + sha}, {"source", "GitDiff"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(content["source"] == "GitDiff");
    AISTUDIO_EXPECT(content["sha"] == sha);
    AISTUDIO_EXPECT(content["content"].get<std::string>().find("+void Foo()") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceGitDiff_MalformedId_IsError) {
    TempProject project;
    BackendRegistry registry;
    const McpServer server(McpServerOptions{.backend_registry = &registry, .project_root = project.RootString()});

    const auto response =
        ToolCallResult(server, "context_fetch", Json{{"id", "git/commit/not-hex!!"}, {"source", "GitDiff"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceCustom_KeywordId_ReturnsSurroundingLines) {
    TempProject project;
    project.WriteFile("notes.txt", "l1\nl2\nl3\nl4\nl5\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response =
        ToolCallResult(server, "context_fetch", Json{{"id", "keyword:notes.txt:3"}, {"source", "Custom"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(content["source"] == "Custom");
    AISTUDIO_EXPECT(content["resolved_file"] == "notes.txt");
    AISTUDIO_EXPECT(content["content"].get<std::string>().find("l3") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ContextFetch_SourceCustom_NonKeywordId_IsError) {
    TempProject project;
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response =
        ToolCallResult(server, "context_fetch", Json{{"id", "rules/project"}, {"source", "Custom"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_UnknownSource_IsError) {
    TempProject project;
    project.WriteFile("notes.txt", "hello\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response =
        ToolCallResult(server, "context_fetch", Json{{"id", "notes.txt"}, {"source", "NotAThing"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_NonAsciiId_ResolvesToCorrectFileContent) {
    // Regression test for the same class of bug fixed in WorkspaceHash()/
    // Sandbox::Check()/EditorStateStore (docs/ROADMAP.md "重大バグ発見・
    // 修正..."): ResolveProjectFile() (this file's anonymous namespace)
    // constructs fs::path directly from `id`, which is a connected AI's
    // own tool-call argument. Written with Utf8ToPath() (not
    // TempProject::WriteFile(), whose own fs::path '/' operator on a
    // std::string has this exact same bug -- confirmed while writing the
    // EditorStateStore regression test) so the file actually exists,
    // letting this test check for silent misresolution (a clean but
    // wrong "not found") in addition to a crash -- this specific input
    // was checked to NOT throw (whether fs::path's CP_ACP round-trip
    // throws depends on whether the corrupted byte sequence happens to
    // be invalid for CP_ACP, not something to rely on), so only checking
    // "didn't throw" would silently pass even if the file were
    // unreachable through this codepath.
    TempProject project;
    const std::string filename = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88.txt"; // 日本語テスト.txt
    {
        std::ofstream out(project.root / Utf8ToPath(filename), std::ios::binary);
        out << "hello from a non-ascii filename\n";
    }
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto content = ParsedContent(ToolCallResult(server, "context_fetch", Json{{"id", filename}}));
    AISTUDIO_EXPECT(content["content"] == "hello from a non-ascii filename\n");
}

AISTUDIO_TEST(McpServer_ContextFetch_NoSourceGiven_DefaultsToFile_ExistingBehaviorUnchanged) {
    TempProject project;
    project.WriteFile("notes.txt", "hello\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto content = ParsedContent(ToolCallResult(server, "context_fetch", Json{{"id", "notes.txt"}}));
    AISTUDIO_EXPECT(content["content"] == "hello\n");
}

AISTUDIO_TEST(McpServer_ContextFetch_Full_ReturnsWholeFile) {
    TempProject project;
    project.WriteFile("notes.txt", "line one\nline two\nline three\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response = ToolCallResult(server, "context_fetch", Json{{"id", "notes.txt"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == false);
    const auto content = ParsedContent(response);

    AISTUDIO_EXPECT(content["mode"] == "full");
    AISTUDIO_EXPECT(content["compression"] == "Raw");
    AISTUDIO_EXPECT(content["total_lines"] == 3);
    AISTUDIO_EXPECT(content["content"] == "line one\nline two\nline three\n");
}

// docs/ROADMAP.md CE-5: a binary file's raw bytes can't become JSON
// string content -- this used to crash the whole response (invalid
// UTF-8 at JSON serialization) instead of returning a clean tool error.
AISTUDIO_TEST(McpServer_ContextFetch_BinaryFile_IsErrorNotCrash) {
    TempProject project;
    std::string binary_content = "some text";
    binary_content += '\xFA';
    binary_content += '\x17';
    binary_content += "more bytes";
    project.WriteFile("asset.bin", binary_content);
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response = ToolCallResult(server, "context_fetch", Json{{"id", "asset.bin"}});
    AISTUDIO_EXPECT(response["result"].value("isError", false) == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_Range_ReturnsOnlyRequestedLines) {
    TempProject project;
    project.WriteFile("notes.txt", "l1\nl2\nl3\nl4\nl5\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto content = ParsedContent(ToolCallResult(
        server, "context_fetch", Json{{"id", "notes.txt"}, {"mode", "range"}, {"start_line", 2}, {"end_line", 3}}));

    AISTUDIO_EXPECT(content["content"] == "l2\nl3");
    AISTUDIO_EXPECT(content["compression"] == "Summary");
    AISTUDIO_EXPECT(content["start_line"] == 2);
    AISTUDIO_EXPECT(content["end_line"] == 3);
    AISTUDIO_EXPECT(content["total_lines"] == 5);
}

AISTUDIO_TEST(McpServer_ContextFetch_Range_MissingLineArguments_IsError) {
    TempProject project;
    project.WriteFile("notes.txt", "l1\nl2\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response =
        ToolCallResult(server, "context_fetch", Json{{"id", "notes.txt"}, {"mode", "range"}, {"start_line", 1}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_UnknownMode_IsError) {
    TempProject project;
    project.WriteFile("notes.txt", "l1\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response = ToolCallResult(server, "context_fetch", Json{{"id", "notes.txt"}, {"mode", "symbol"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_MaxTokens_CompressesInsteadOfTruncating) {
    TempProject project;
    project.WriteFile("big.txt", std::string(8000, 'x'));
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto content =
        ParsedContent(ToolCallResult(server, "context_fetch", Json{{"id", "big.txt"}, {"max_tokens", 100}}));

    AISTUDIO_EXPECT(content["compressed"] == true);
    AISTUDIO_EXPECT(content["estimated_tokens"].get<std::int64_t>() <= 100);
    AISTUDIO_EXPECT(content["content"].get<std::string>().find("omitted") != std::string::npos);
}

AISTUDIO_TEST(McpServer_ContextFetch_NonFileId_IsErrorRatherThanAGuess) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    // Symbol-source id, not a file path: error beats fetching Player.hpp.
    const auto response = ToolCallResult(server, "context_fetch", Json{{"id", "Player.hpp:PlayerAttack"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_PathTraversal_IsErrorEvenWithoutAFirewall) {
    TempProject project;
    project.WriteFile("inside.txt", "ok\n");
    // No firewall: the tool must still refuse to leave project_root.
    const McpServer server(McpServerOptions{.project_root = project.RootString()});

    const auto response = ToolCallResult(server, "context_fetch", Json{{"id", "../../Windows/win.ini"}});
    AISTUDIO_EXPECT(response["result"]["isError"] == true);
}

AISTUDIO_TEST(McpServer_ContextFetch_FirewallDeniedFile_IsError) {
    TempProject project;
    project.WriteFile("secret.key", "s3cret\n");
    project.WriteFile("open.txt", "fine\n");
    const Sandbox firewall(project.RootString(), Sandbox::Options{.deny_patterns = {"*.key"}});
    const McpServer server(McpServerOptions{.project_root = project.RootString(), .firewall = &firewall});

    AISTUDIO_EXPECT(ToolCallResult(server, "context_fetch", Json{{"id", "secret.key"}})["result"]["isError"] == true);
    AISTUDIO_EXPECT(
        ToolCallResult(server, "context_fetch", Json{{"id", "open.txt"}})["result"].value("isError", false) == false);
}

AISTUDIO_TEST(McpServer_ContextFetch_NoProjectRoot_IsErrorAndToolIsNotListed) {
    const McpServer server(McpServerOptions{});
    AISTUDIO_EXPECT(ToolCallResult(server, "context_fetch", Json{{"id", "a.txt"}})["result"]["isError"] == true);

    const auto response = RunOne(server, R"({"jsonrpc":"2.0","id":9,"method":"tools/list"})");
    for (const auto& tool : response["result"]["tools"]) {
        AISTUDIO_EXPECT(tool["name"] != "context_fetch");
    }
}
