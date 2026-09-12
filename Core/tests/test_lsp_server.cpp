#include "test_framework.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/LSP/LspServer.hpp"
#include "Core/Project/FileWatcher.hpp"
#include "Core/Security/Sandbox.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <any>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_lsp_server_test_" + std::to_string(counter++));
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

// Encodes one JSON-RPC message with LSP's own Content-Length framing.
std::string Frame(const Json& message) {
    const std::string body = message.dump();
    std::ostringstream out;
    out << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    return out.str();
}

// Feeds every already-framed message in `messages` through one
// LspServer::Run() call and returns every framed response, parsed --
// same "concatenate input, run once, split output" shape as
// test_mcp_server.cpp's own RunLines(), adapted for Content-Length
// framing instead of newline-delimited.
std::vector<Json> RunFrames(const LspServer& server, const std::vector<Json>& messages) {
    std::ostringstream input;
    for (const auto& message : messages) {
        input << Frame(message);
    }
    std::istringstream in(input.str());
    std::ostringstream out;
    server.Run(in, out);

    std::vector<Json> responses;
    const std::string output = out.str();
    static const std::string kContentLengthHeader = "Content-Length:";
    std::size_t pos = 0;
    while (pos < output.size()) {
        const auto header_end = output.find("\r\n\r\n", pos);
        if (header_end == std::string::npos) {
            break;
        }
        const std::string headers = output.substr(pos, header_end - pos);
        const auto length_pos = headers.find(kContentLengthHeader);
        AISTUDIO_EXPECT(length_pos != std::string::npos);
        const auto length = static_cast<std::size_t>(std::stoul(headers.substr(length_pos + kContentLengthHeader.size())));
        const auto body_start = header_end + 4;
        responses.push_back(Json::parse(output.substr(body_start, length)));
        pos = body_start + length;
    }
    return responses;
}

Json RunOne(const LspServer& server, const Json& message) {
    const auto responses = RunFrames(server, {message});
    AISTUDIO_EXPECT(responses.size() == 1);
    return responses.front();
}

Json MakeInitialize(int id) {
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "initialize"}, {"params", Json::object()}};
}

Json MakeDidOpen(const std::string& uri, const std::string& text) {
    return Json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didOpen"},
        {"params", Json{{"textDocument", Json{{"uri", uri}, {"languageId", "cpp"}, {"version", 1}, {"text", text}}}}},
    };
}

Json MakeDidChange(const std::string& uri, const std::string& text) {
    return Json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didChange"},
        {"params", Json{{"textDocument", Json{{"uri", uri}, {"version", 2}}},
                         {"contentChanges", Json::array({Json{{"text", text}}})}}},
    };
}

Json MakeDidClose(const std::string& uri) {
    return Json{
        {"jsonrpc", "2.0"},
        {"method", "textDocument/didClose"},
        {"params", Json{{"textDocument", Json{{"uri", uri}}}}},
    };
}

Json MakeDefinitionRequest(int id, const std::string& uri, int line, int character) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "textDocument/definition"},
        {"params",
         Json{{"textDocument", Json{{"uri", uri}}}, {"position", Json{{"line", line}, {"character", character}}}}},
    };
}

Json MakeTypeDefinitionRequest(int id, const std::string& uri, int line, int character) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "textDocument/typeDefinition"},
        {"params",
         Json{{"textDocument", Json{{"uri", uri}}}, {"position", Json{{"line", line}, {"character", character}}}}},
    };
}

Json MakeDocumentSymbolRequest(int id, const std::string& uri) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "textDocument/documentSymbol"},
        {"params", Json{{"textDocument", Json{{"uri", uri}}}}},
    };
}

Json MakeWorkspaceSymbolRequest(int id, const std::string& query) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "workspace/symbol"},
        {"params", Json{{"query", query}}},
    };
}

Json MakeReferencesRequest(int id, const std::string& uri, int line, int character,
                            std::optional<bool> include_declaration = std::nullopt) {
    Json params{{"textDocument", Json{{"uri", uri}}}, {"position", Json{{"line", line}, {"character", character}}}};
    if (include_declaration.has_value()) {
        params["context"] = Json{{"includeDeclaration", *include_declaration}};
    }
    return Json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "textDocument/references"}, {"params", params}};
}

Json MakeRenameRequest(int id, const std::string& uri, int line, int character, const Json& new_name) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "textDocument/rename"},
        {"params",
         Json{{"textDocument", Json{{"uri", uri}}},
              {"position", Json{{"line", line}, {"character", character}}},
              {"newName", new_name}}},
    };
}

Json MakePrepareRenameRequest(int id, const std::string& uri, int line, int character) {
    return Json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", "textDocument/prepareRename"},
        {"params",
         Json{{"textDocument", Json{{"uri", uri}}}, {"position", Json{{"line", line}, {"character", character}}}}},
    };
}

// URI for a path that need not exist on disk -- LspServer never resolves
// an incoming document's own URI back to a filesystem path (see
// LspServer.cpp's own comment on this), so tests can use any well-formed
// file:// URI as a document's identity.
std::string FileUri(const fs::path& path) {
    const std::string generic = path.generic_string();
    std::string uri = "file://";
#if defined(_WIN32)
    if (!generic.empty() && generic[0] != '/') {
        uri += '/';
    }
#endif
    uri += generic;
    return uri;
}

} // namespace

AISTUDIO_TEST(LspServer_Initialize_ReturnsCapabilitiesAndServerInfo) {
    const LspServer server(LspServerOptions{.server_name = "aistudio-core-lsp", .server_version = "0.1.0"});
    const auto response = RunOne(server, MakeInitialize(1));

    AISTUDIO_EXPECT(response["id"] == 1);
    AISTUDIO_EXPECT(response["result"]["serverInfo"]["name"] == "aistudio-core-lsp");
    AISTUDIO_EXPECT(response["result"]["serverInfo"]["version"] == "0.1.0");
    AISTUDIO_EXPECT(response["result"]["capabilities"]["textDocumentSync"]["openClose"] == true);
    AISTUDIO_EXPECT(response["result"]["capabilities"]["textDocumentSync"]["change"] == 1);
    // No SymbolIndex/project_root configured -- no definitionProvider advertised.
    AISTUDIO_EXPECT(!response["result"]["capabilities"].contains("definitionProvider"));
}

AISTUDIO_TEST(LspServer_Initialize_WithSymbolIndex_AdvertisesDefinitionProvider) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(response["result"]["capabilities"]["definitionProvider"] == true);
}

AISTUDIO_TEST(LspServer_Initialize_WithSymbolIndex_AdvertisesDocumentSymbolAndWorkspaceSymbolProviders) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(response["result"]["capabilities"]["documentSymbolProvider"] == true);
    AISTUDIO_EXPECT(response["result"]["capabilities"]["workspaceSymbolProvider"] == true);
}

AISTUDIO_TEST(LspServer_Initialize_NoSymbolIndex_DoesNotAdvertiseDocumentSymbolOrWorkspaceSymbolProviders) {
    const LspServer server(LspServerOptions{});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(!response["result"]["capabilities"].contains("documentSymbolProvider"));
    AISTUDIO_EXPECT(!response["result"]["capabilities"].contains("workspaceSymbolProvider"));
    AISTUDIO_EXPECT(!response["result"]["capabilities"].contains("referencesProvider"));
    AISTUDIO_EXPECT(!response["result"]["capabilities"].contains("typeDefinitionProvider"));
}

AISTUDIO_TEST(LspServer_Initialize_WithSymbolIndex_AdvertisesReferencesProvider) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(response["result"]["capabilities"]["referencesProvider"] == true);
}

AISTUDIO_TEST(LspServer_Initialize_WithSymbolIndex_AdvertisesTypeDefinitionProvider) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(response["result"]["capabilities"]["typeDefinitionProvider"] == true);
}

AISTUDIO_TEST(LspServer_Shutdown_ReturnsNullResult) {
    const LspServer server(LspServerOptions{});
    const auto response = RunOne(server, Json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "shutdown"}});
    AISTUDIO_EXPECT(response["id"] == 2);
    AISTUDIO_EXPECT(response["result"].is_null());
}

AISTUDIO_TEST(LspServer_RequestAfterShutdown_IsRejected) {
    const LspServer server(LspServerOptions{});
    const auto responses = RunFrames(server, {
                                                  Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "shutdown"}},
                                                  MakeInitialize(2),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32600);
}

AISTUDIO_TEST(LspServer_Exit_EndsTheSessionWithNoResponse) {
    const LspServer server(LspServerOptions{});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  Json{{"jsonrpc", "2.0"}, {"method", "exit"}},
                                                  MakeInitialize(2), // never processed -- Run() already returned
                                              });
    AISTUDIO_EXPECT(responses.size() == 1);
}

AISTUDIO_TEST(LspServer_UnknownMethod_ReturnsMethodNotFound) {
    const LspServer server(LspServerOptions{});
    const auto response = RunOne(server, Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "textDocument/hover"}});
    AISTUDIO_EXPECT(response["error"]["code"] == -32601);
}

AISTUDIO_TEST(LspServer_Notification_ProducesNoResponse) {
    const LspServer server(LspServerOptions{});
    const auto responses = RunFrames(server, {Json{{"jsonrpc", "2.0"}, {"method", "initialized"}}});
    AISTUDIO_EXPECT(responses.empty());
}

AISTUDIO_TEST(LspServer_RequestWithoutId_ProducesNoResponse) {
    // Per JSON-RPC 2.0, absence of "id" makes any message a Notification,
    // not just ones LspServer recognizes by name.
    const LspServer server(LspServerOptions{});
    const auto responses = RunFrames(server, {Json{{"jsonrpc", "2.0"}, {"method", "textDocument/definition"}}});
    AISTUDIO_EXPECT(responses.empty());
}

AISTUDIO_TEST(LspServer_MalformedJson_ReturnsParseError) {
    const LspServer server(LspServerOptions{});
    const std::string body = "not valid json {";
    std::ostringstream framed;
    framed << "Content-Length: " << body.size() << "\r\n\r\n" << body;
    std::istringstream in(framed.str());
    std::ostringstream out;
    server.Run(in, out);
    AISTUDIO_EXPECT(out.str().find("-32700") != std::string::npos);
}

AISTUDIO_TEST(LspServer_NegativeContentLength_RejectedCleanlyInsteadOfCrashing) {
    // Regression test for a real bug: std::stoul("-1") does NOT throw --
    // per strtoul() semantics it silently wraps around to a huge unsigned
    // value (near ULONG_MAX). ReadMessage() used to hand that value
    // straight to `std::string body(content_length, '\0')` outside any
    // try/catch, so a single "Content-Length: -1" frame took down the
    // whole --lsp process with an uncaught std::bad_alloc/length_error.
    // If this test's process survives running it at all, the crash is
    // already fixed; the assertions below additionally confirm the frame
    // is refused the same clean way ReadMessage() already refuses any
    // other malformed framing (no response written, session ends) rather
    // than being silently accepted.
    const LspServer server(LspServerOptions{});
    const std::string body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})";
    std::ostringstream framed;
    framed << "Content-Length: -1\r\n\r\n" << body;
    std::istringstream in(framed.str());
    std::ostringstream out;
    server.Run(in, out); // must not crash / hang
    AISTUDIO_EXPECT(out.str().empty()); // malformed framing -- rejected before any response, same as other bad frames
}

AISTUDIO_TEST(LspServer_ExcessivelyLargeContentLength_RejectedCleanlyInsteadOfAllocating) {
    // Same bug class as the negative-Content-Length test above, but via a
    // value that's technically parseable by std::stoul (no exception, no
    // unsigned wraparound) yet still far larger than any real LSP message
    // could legitimately be -- must be rejected by the explicit
    // kMaxContentLength bound check before ReadMessage() ever attempts to
    // allocate a body string that size.
    const LspServer server(LspServerOptions{});
    std::ostringstream framed;
    framed << "Content-Length: 500000000\r\n\r\n"; // 500 MB -- well past the 64 MiB bound, no body bytes follow
    std::istringstream in(framed.str());
    std::ostringstream out;
    server.Run(in, out); // must not crash / hang / attempt the allocation
    AISTUDIO_EXPECT(out.str().empty());
}

AISTUDIO_TEST(LspServer_Definition_FindsFunctionDefinitionAtCursor) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) {\n    return x + 1;\n}\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    AISTUDIO_EXPECT(symbol_index.Size() > 0);

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "caller.cpp");
    // Line 1 (0-based): "    Compute(5);" -- character 6 lands inside "Compute".
    const std::string caller_text = "void Caller() {\n    Compute(5);\n}\n";

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, caller_text),
                                                  MakeDefinitionRequest(2, uri, 1, 6),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& definition_response = responses[1];
    AISTUDIO_EXPECT(definition_response["id"] == 2);
    AISTUDIO_EXPECT(definition_response["result"].is_array());
    AISTUDIO_EXPECT(definition_response["result"].size() == 1);
    const auto& location = definition_response["result"][0];
    AISTUDIO_EXPECT(location["uri"].get<std::string>().find("foo.cpp") != std::string::npos);
    AISTUDIO_EXPECT(location["range"]["start"]["line"] == 0);
}

AISTUDIO_TEST(LspServer_Definition_UnopenedDocument_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDefinitionRequest(2, FileUri(project.root / "never_opened.cpp"), 0, 4),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_Definition_UnknownIdentifier_ReturnsNull) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = NoSuchSymbol();\n"),
                                                  MakeDefinitionRequest(2, uri, 0, 10),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_Definition_FirewallExcludesDeniedFile) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns (same fixture
    // shape as McpServer_ToolsCall_SymbolSearch_FirewallBlocksMatch in
    // test_mcp_server.cpp).
    project.WriteFile("my_secret.hpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const Sandbox firewall(project.RootString());
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .firewall = &firewall});

    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int y = Compute(1);\n"),
                                                  MakeDefinitionRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_Definition_AfterDidChange_UsesUpdatedContent) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    project.WriteFile("bar.cpp", "int Other(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int a = Compute(1);\n"),
                                                  MakeDidChange(uri, "int b = Other(1);\n"),
                                                  MakeDefinitionRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& location = responses[1]["result"][0];
    AISTUDIO_EXPECT(location["uri"].get<std::string>().find("bar.cpp") != std::string::npos);
}

AISTUDIO_TEST(LspServer_Definition_AfterDidClose_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int a = Compute(1);\n"),
                                                  MakeDidClose(uri),
                                                  MakeDefinitionRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_Definition_NoSymbolIndexConfigured_ReturnsNull) {
    const LspServer server(LspServerOptions{});
    const std::string uri = FileUri(fs::temp_directory_path() / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int a = Compute(1);\n"),
                                                  MakeDefinitionRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

// ---------------------------------------------------------------------
// textDocument/typeDefinition
// ---------------------------------------------------------------------

AISTUDIO_TEST(LspServer_TypeDefinition_VariableUsage_ResolvesToDeclaredTypeClass) {
    TempProject project;
    project.WriteFile("widget.hpp", "class Widget {\n};\n");
    project.WriteFile("instance.cpp", "Widget global_widget;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    // Line 1 (0-based): "    global_widget.Reset();" -- character 6 lands inside "global_widget".
    const std::string caller_text = "void Caller() {\n    global_widget.Reset();\n}\n";

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, caller_text),
                                                  MakeTypeDefinitionRequest(2, uri, 1, 6),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());
    AISTUDIO_EXPECT(result.size() == 1);
    AISTUDIO_EXPECT(result[0]["uri"].get<std::string>().find("widget.hpp") != std::string::npos);
}

AISTUDIO_TEST(LspServer_TypeDefinition_FunctionCall_ResolvesToReturnTypeClass) {
    TempProject project;
    project.WriteFile("widget.hpp", "class Widget {\n};\n");
    project.WriteFile("factory.cpp", "Widget Create() {\n    return Widget();\n}\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const std::string caller_text = "void Caller() {\n    Create();\n}\n";

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, caller_text),
                                                  MakeTypeDefinitionRequest(2, uri, 1, 6),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());
    AISTUDIO_EXPECT(result.size() == 1);
    AISTUDIO_EXPECT(result[0]["uri"].get<std::string>().find("widget.hpp") != std::string::npos);
}

AISTUDIO_TEST(LspServer_TypeDefinition_PrimitiveReturnType_ReturnsNull) {
    // "int" parses as primitive_type -- Symbol::type_name stays empty, so
    // there is nothing to look up (not a guess).
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "void Caller() {\n    Compute(5);\n}\n"),
                                                  MakeTypeDefinitionRequest(2, uri, 1, 6),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_TypeDefinition_UnknownIdentifier_ReturnsNull) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = NoSuchSymbol();\n"),
                                                  MakeTypeDefinitionRequest(2, uri, 0, 10),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_TypeDefinition_UnopenedDocument_ReturnsNull) {
    TempProject project;
    project.WriteFile("widget.hpp", "class Widget {\n};\n");
    project.WriteFile("instance.cpp", "Widget global_widget;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeTypeDefinitionRequest(2, FileUri(project.root / "never_opened.cpp"), 0, 4),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_TypeDefinition_FirewallExcludesDeniedTypeFile) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns (same fixture
    // shape as LspServer_Definition_FirewallExcludesDeniedFile).
    project.WriteFile("my_secret.hpp", "class Widget {\n};\n");
    project.WriteFile("instance.cpp", "Widget global_widget;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const Sandbox firewall(project.RootString());
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .firewall = &firewall});

    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "void Caller() {\n    global_widget.Reset();\n}\n"),
                                                  MakeTypeDefinitionRequest(2, uri, 1, 6),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_TypeDefinition_NoSymbolIndexConfigured_ReturnsNull) {
    const LspServer server(LspServerOptions{});
    const std::string uri = FileUri(fs::temp_directory_path() / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "Widget global_widget;\n"),
                                                  MakeTypeDefinitionRequest(2, uri, 0, 4),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

// ---------------------------------------------------------------------
// textDocument/documentSymbol
// ---------------------------------------------------------------------

AISTUDIO_TEST(LspServer_DocumentSymbol_ReturnsOnlySymbolsFromTheRequestedFile) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) {\n    return x + 1;\n}\n\nstruct Widget {\n    void Reset();\n};\n");
    project.WriteFile("bar.cpp", "int Other(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDocumentSymbolRequest(2, FileUri(project.root / "foo.cpp")),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());
    // "Compute" (Function) and "Widget" (Struct) both live in foo.cpp --
    // "Other" (bar.cpp) must not appear.
    AISTUDIO_EXPECT(result.size() == 2);
    bool found_compute = false;
    bool found_widget = false;
    for (const auto& symbol_info : result) {
        AISTUDIO_EXPECT(symbol_info.contains("location"));
        if (symbol_info["name"] == "Compute") {
            found_compute = true;
            AISTUDIO_EXPECT(symbol_info["kind"] == 12); // Function
        } else if (symbol_info["name"] == "Widget") {
            found_widget = true;
            AISTUDIO_EXPECT(symbol_info["kind"] == 23); // Struct
        }
    }
    AISTUDIO_EXPECT(found_compute);
    AISTUDIO_EXPECT(found_widget);
}

AISTUDIO_TEST(LspServer_DocumentSymbol_LocationFindsWholeWordNotPrefixMatch) {
    // Regression test: LocationFor's column search used to be a plain
    // substring find(), which could match a symbol's name as a mere
    // PREFIX of an earlier, longer identifier on the same line rather
    // than the symbol's own occurrence -- e.g. "Compute" matching inside
    // "ComputeAll" here. FindWholeWord() (Core/src/LSP/LspServer.cpp)
    // fixes this by requiring identifier-character boundaries on both
    // sides of the match.
    TempProject project;
    project.WriteFile("foo.cpp", "int ComputeAll = 0; int Compute = 1;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDocumentSymbolRequest(2, FileUri(project.root / "foo.cpp")),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());

    bool found_compute = false;
    for (const auto& symbol_info : result) {
        if (symbol_info["name"] == "Compute") {
            found_compute = true;
            // "Compute" (the real variable) starts at byte/UTF-16 offset
            // 24 on this line -- NOT offset 4, which is where a naive
            // substring search would incorrectly match inside
            // "ComputeAll" instead.
            AISTUDIO_EXPECT(symbol_info["location"]["range"]["start"]["character"] == 24);
        }
    }
    AISTUDIO_EXPECT(found_compute);
}

AISTUDIO_TEST(LspServer_DocumentSymbol_DoesNotRequireTheDocumentToBeOpen) {
    // SymbolIndex is disk-based, not buffer-based -- unlike
    // textDocument/definition, documentSymbol doesn't need a prior
    // didOpen for the requested file.
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDocumentSymbolRequest(2, FileUri(project.root / "foo.cpp")),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_array());
    AISTUDIO_EXPECT(responses[1]["result"].size() == 1);
    AISTUDIO_EXPECT(responses[1]["result"][0]["name"] == "Compute");
}

AISTUDIO_TEST(LspServer_DocumentSymbol_FileWithNoSymbols_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDocumentSymbolRequest(2, FileUri(project.root / "empty.cpp")),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_DocumentSymbol_UriOutsideProjectRoot_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDocumentSymbolRequest(
                                                      2, FileUri(fs::temp_directory_path() / "elsewhere" / "foo.cpp")),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_DocumentSymbol_FirewallExcludesDeniedFile) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns (same fixture
    // shape as LspServer_Definition_FirewallExcludesDeniedFile above).
    project.WriteFile("my_secret.hpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const Sandbox firewall(project.RootString());
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .firewall = &firewall});

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDocumentSymbolRequest(2, FileUri(project.root / "my_secret.hpp")),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_DocumentSymbol_NoSymbolIndexConfigured_ReturnsNull) {
    const LspServer server(LspServerOptions{});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDocumentSymbolRequest(
                                                      2, FileUri(fs::temp_directory_path() / "foo.cpp")),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

// ---------------------------------------------------------------------
// workspace/symbol
// ---------------------------------------------------------------------

AISTUDIO_TEST(LspServer_WorkspaceSymbol_FindsMatchesAcrossTheWholeProject) {
    TempProject project;
    project.WriteFile("foo.cpp", "int ComputeTotal(int x) { return x; }\n");
    project.WriteFile("bar.cpp", "int ComputeAverage(int x) { return x; }\n");
    project.WriteFile("baz.cpp", "int Unrelated(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeWorkspaceSymbolRequest(2, "Compute"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());
    AISTUDIO_EXPECT(result.size() == 2);
    bool found_total = false;
    bool found_average = false;
    for (const auto& symbol_info : result) {
        if (symbol_info["name"] == "ComputeTotal") {
            found_total = true;
        } else if (symbol_info["name"] == "ComputeAverage") {
            found_average = true;
        }
        AISTUDIO_EXPECT(symbol_info["name"] != "Unrelated");
    }
    AISTUDIO_EXPECT(found_total);
    AISTUDIO_EXPECT(found_average);
}

AISTUDIO_TEST(LspServer_WorkspaceSymbol_EmptyQuery_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeWorkspaceSymbolRequest(2, ""),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_WorkspaceSymbol_NoMatches_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeWorkspaceSymbolRequest(2, "NoSuchSymbolAnywhere"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_WorkspaceSymbol_FirewallExcludesDeniedFile) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "int Compute(int x) { return x; }\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const Sandbox firewall(project.RootString());
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .firewall = &firewall});

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeWorkspaceSymbolRequest(2, "Compute"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_WorkspaceSymbol_NoSymbolIndexConfigured_ReturnsNull) {
    const LspServer server(LspServerOptions{});
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeWorkspaceSymbolRequest(2, "Compute"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

// ---------------------------------------------------------------------
// textDocument/references
//
// This is a name-text heuristic (whole-word, case-sensitive scan across
// every source file under project_root), NOT built on ReferenceGraph/
// CallGraph -- see HandleReferences's own comment in LspServer.cpp for
// the investigation behind that choice. The "Total" fixture below is
// deliberately a VARIABLE (SymbolKind::Variable), not a type or a
// function -- neither ReferenceGraph (type_identifier only) nor CallGraph
// (call_expression callees only) would find any of these occurrences at
// all, which is exactly the coverage gap that ruled those two indexes
// out for this handler.
// ---------------------------------------------------------------------

AISTUDIO_TEST(LspServer_References_FindsWholeWordOccurrencesAcrossTheWholeProjectIncludingDeclaration) {
    TempProject project;
    // Column layout (0-based):
    //   foo.cpp line0 "int Total = 0;"        -> "Total" at col 4  (the declaration)
    //   foo.cpp line2 "    Total = Total + 1;" -> "Total" at col 4 and col 12
    //   bar.cpp line1 "    Total = 5;"         -> "Total" at col 4
    project.WriteFile("foo.cpp", "int Total = 0;\nvoid Foo() {\n    Total = Total + 1;\n}\n");
    project.WriteFile("bar.cpp", "void Bar() {\n    Total = 5;\n}\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeReferencesRequest(2, uri, 0, 9), // cursor inside "Total"
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());
    AISTUDIO_EXPECT(result.size() == 4); // declaration + 2 in foo.cpp + 1 in bar.cpp

    int foo_hits = 0;
    int bar_hits = 0;
    bool found_declaration = false;
    for (const auto& location : result) {
        const std::string location_uri = location["uri"].get<std::string>();
        const int start_line = location["range"]["start"]["line"].get<int>();
        const int start_char = location["range"]["start"]["character"].get<int>();
        if (location_uri.find("foo.cpp") != std::string::npos) {
            ++foo_hits;
            if (start_line == 0 && start_char == 4) {
                found_declaration = true;
            }
        } else if (location_uri.find("bar.cpp") != std::string::npos) {
            ++bar_hits;
            AISTUDIO_EXPECT(start_line == 1);
            AISTUDIO_EXPECT(start_char == 4);
        }
    }
    AISTUDIO_EXPECT(foo_hits == 3);
    AISTUDIO_EXPECT(bar_hits == 1);
    AISTUDIO_EXPECT(found_declaration);
}

AISTUDIO_TEST(LspServer_References_IncludeDeclarationFalse_ExcludesTheDeclarationSite) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\nvoid Foo() {\n    Total = Total + 1;\n}\n");
    project.WriteFile("bar.cpp", "void Bar() {\n    Total = 5;\n}\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeReferencesRequest(2, uri, 0, 9, /*include_declaration=*/false),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());
    AISTUDIO_EXPECT(result.size() == 3); // declaration (foo.cpp line0 col4) excluded

    for (const auto& location : result) {
        const std::string location_uri = location["uri"].get<std::string>();
        const int start_line = location["range"]["start"]["line"].get<int>();
        const int start_char = location["range"]["start"]["character"].get<int>();
        const bool is_declaration_site = location_uri.find("foo.cpp") != std::string::npos && start_line == 0 && start_char == 4;
        AISTUDIO_EXPECT(!is_declaration_site);
    }
}

AISTUDIO_TEST(LspServer_References_DoesNotMatchPartialWordSubstring) {
    // "Compute" must not match inside "ComputeAll" -- same whole-word
    // boundary rule LocationFor's own regression test already covers for
    // documentSymbol.
    TempProject project;
    project.WriteFile("foo.cpp", "int ComputeAll = 0;\nint Compute = 1;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int y = Compute;\n"),
                                                  MakeReferencesRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.is_array());
    // Only "Compute"'s own declaration (foo.cpp line1 col4) -- NOT a match
    // inside "ComputeAll" on line0.
    AISTUDIO_EXPECT(result.size() == 1);
    AISTUDIO_EXPECT(result[0]["range"]["start"]["line"] == 1);
    AISTUDIO_EXPECT(result[0]["range"]["start"]["character"] == 4);
}

AISTUDIO_TEST(LspServer_References_UnopenedDocument_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeReferencesRequest(2, FileUri(project.root / "never_opened.cpp"), 0, 4),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_References_UnknownIdentifier_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = NoSuchSymbolAnywhere();\n"),
                                                  MakeReferencesRequest(2, uri, 0, 10),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_References_FirewallExcludesDeniedFile) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns (same fixture
    // shape as LspServer_Definition_FirewallExcludesDeniedFile above).
    project.WriteFile("my_secret.hpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const Sandbox firewall(project.RootString());
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .firewall = &firewall});

    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeReferencesRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null()); // the only occurrence lives in the firewalled file
}

AISTUDIO_TEST(LspServer_References_NoSymbolIndexConfigured_ReturnsNull) {
    const LspServer server(LspServerOptions{});
    const std::string uri = FileUri(fs::temp_directory_path() / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeReferencesRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

// ---------------------------------------------------------------------
// textDocument/publishDiagnostics -- docs/ROADMAP.md's Diagnostics
// Design Proposal, resolved by the user's decision: v1 scoped to
// INDEX-DERIVED diagnostics only (parse failures + unresolved
// `#include`s), no real compiler/build execution. Unlike every test
// above, this is a server-initiated NOTIFICATION (no "id" field) rather
// than a response to a specific request -- RunFrames already captures it
// the same way (it doesn't filter by whether a message has an "id"), so
// no new test-harness plumbing is needed; see LspServer.cpp's own
// PublishDiagnosticsIfChanged comment for why most of the didOpen calls
// used by every OTHER test in this file never emit one (all use
// well-formed C++ with no #include directives at all, so their
// diagnostics list is always empty and this feature's "quiet unless
// something changed" design sends nothing -- confirmed empirically by
// this whole file's pre-existing tests staying byte-for-byte unaffected
// by this feature's addition).
// ---------------------------------------------------------------------

AISTUDIO_TEST(LspServer_PublishDiagnostics_ParseError_OnDidOpen_SendsErrorSeverityNotification) {
    TempProject project;
    const LspServer server(LspServerOptions{.project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "broken.cpp");
    // Unclosed brace -- tree-sitter-cpp's own error recovery synthesizes
    // a MISSING '}' rather than throwing, which is exactly the signal
    // DetectParseError looks for.
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "void Foo() {\n    int x = 1;\n"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);

    const auto& notification = responses[1];
    AISTUDIO_EXPECT(!notification.contains("id"));
    AISTUDIO_EXPECT(notification["method"] == "textDocument/publishDiagnostics");
    AISTUDIO_EXPECT(notification["params"]["uri"] == uri);
    const auto& diagnostics = notification["params"]["diagnostics"];
    AISTUDIO_EXPECT(diagnostics.is_array());
    AISTUDIO_EXPECT(diagnostics.size() == 1);
    AISTUDIO_EXPECT(diagnostics[0]["severity"] == 1); // Error
    AISTUDIO_EXPECT(diagnostics[0]["source"] == "aistudio-index");
    AISTUDIO_EXPECT(diagnostics[0].contains("range"));
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_WellFormedFile_SendsNoNotification) {
    TempProject project;
    const LspServer server(LspServerOptions{.project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = 1;\n"),
                                              });
    // Only the initialize response -- no publishDiagnostics notification
    // for a well-formed file with nothing to report (see this test
    // group's own comment on the "quiet unless something changed" design).
    AISTUDIO_EXPECT(responses.size() == 1);
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_NoProjectRootConfigured_SendsNoNotification) {
    const LspServer server(LspServerOptions{});
    const std::string uri = FileUri(fs::temp_directory_path() / "broken.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "void Foo() {\n"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 1); // no project_root -- diagnostics gated off entirely
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_FixedOnDidChange_ClearsWithEmptyDiagnostics) {
    TempProject project;
    const LspServer server(LspServerOptions{.project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "broken.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "void Foo() {\n"),
                                                  MakeDidChange(uri, "void Foo() {}\n"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 3);
    AISTUDIO_EXPECT(responses[1]["params"]["diagnostics"].size() == 1); // the parse error on didOpen

    const auto& clear_notification = responses[2];
    AISTUDIO_EXPECT(!clear_notification.contains("id"));
    AISTUDIO_EXPECT(clear_notification["method"] == "textDocument/publishDiagnostics");
    AISTUDIO_EXPECT(clear_notification["params"]["uri"] == uri);
    AISTUDIO_EXPECT(clear_notification["params"]["diagnostics"].is_array());
    AISTUDIO_EXPECT(clear_notification["params"]["diagnostics"].empty());
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_ClearedOnDidClose) {
    TempProject project;
    const LspServer server(LspServerOptions{.project_root = project.RootString()});

    const std::string uri = FileUri(project.root / "broken.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "void Foo() {\n"),
                                                  MakeDidClose(uri),
                                              });
    AISTUDIO_EXPECT(responses.size() == 3);
    AISTUDIO_EXPECT(responses[1]["params"]["diagnostics"].size() == 1);

    const auto& clear_notification = responses[2];
    AISTUDIO_EXPECT(clear_notification["method"] == "textDocument/publishDiagnostics");
    AISTUDIO_EXPECT(clear_notification["params"]["diagnostics"].empty());
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_UnresolvedQuotedInclude_SendsWarningSeverityNotification) {
    TempProject project;
    const std::string content = "#include \"does_not_exist.hpp\"\nint x = 1;\n";
    project.WriteFile("caller.cpp", content);

    IncludeGraph include_graph;
    AISTUDIO_EXPECT(include_graph.Build(project.RootString()));

    const LspServer server(LspServerOptions{.project_root = project.RootString(), .include_graph = &include_graph});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, content),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& diagnostics = responses[1]["params"]["diagnostics"];
    AISTUDIO_EXPECT(diagnostics.size() == 1);
    AISTUDIO_EXPECT(diagnostics[0]["severity"] == 2); // Warning, not Error -- see LspServer.hpp's own comment on why
    AISTUDIO_EXPECT(diagnostics[0]["range"]["start"]["line"] == 0);
    const std::string message = diagnostics[0]["message"].get<std::string>();
    AISTUDIO_EXPECT(message.find("does_not_exist.hpp") != std::string::npos);
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_ResolvedInclude_SendsNoNotification) {
    TempProject project;
    project.WriteFile("included.hpp", "int Helper();\n");
    const std::string content = "#include \"included.hpp\"\nint x = 1;\n";
    project.WriteFile("caller.cpp", content);

    IncludeGraph include_graph;
    AISTUDIO_EXPECT(include_graph.Build(project.RootString()));

    const LspServer server(LspServerOptions{.project_root = project.RootString(), .include_graph = &include_graph});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, content),
                                              });
    AISTUDIO_EXPECT(responses.size() == 1); // resolves cleanly -- nothing to report
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_UnresolvedSystemInclude_SendsNoNotification) {
    TempProject project;
    const std::string content = "#include <vector>\nint x = 1;\n";
    project.WriteFile("caller.cpp", content);

    IncludeGraph include_graph;
    AISTUDIO_EXPECT(include_graph.Build(project.RootString()));

    const LspServer server(LspServerOptions{.project_root = project.RootString(), .include_graph = &include_graph});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, content),
                                              });
    // A system include is never even attempted for resolution by
    // IncludeGraph::Build() itself (its own "if (!edge.is_system)" gate)
    // -- an unresolved <vector> is by-design, not a defect to surface.
    AISTUDIO_EXPECT(responses.size() == 1);
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_NoIncludeGraphConfigured_UnresolvedIncludeNotReported) {
    TempProject project;
    const std::string content = "#include \"does_not_exist.hpp\"\nint x = 1;\n";
    project.WriteFile("caller.cpp", content);

    // include_graph left nullptr -- only the parse-failure half is
    // active, and this content parses cleanly.
    const LspServer server(LspServerOptions{.project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, content),
                                              });
    AISTUDIO_EXPECT(responses.size() == 1);
}

// Regression test for `feature/LspModeFileWatcher` (docs/ROADMAP.md's
// "未解決includeのstale診断" follow-up task, RunLspMode()'s own doc
// comment in Core/src/main.cpp): reproduces the exact staleness bug
// LspServer.hpp's textDocument/publishDiagnostics comment used to
// document as a KNOWN LIMITATION -- an unresolved-`#include` warning
// used to persist forever because `include_graph` was only ever built
// once, never refreshed -- then proves a FileWatcher wired the same way
// RunMcpMode()'s own subscriber rebuilds SymbolIndex/IncludeGraph (see
// that function in main.cpp) fixes it. This constructs the same
// FileWatcher + "FileChanged" subscription + UpdateFile()/RemoveFile()
// shape RunLspMode() now wires directly, rather than invoking
// RunLspMode() itself (that function lives in main.cpp, which isn't
// linked into this test binary -- same reason RunMcpMode()'s own
// FileWatcher wiring has no direct unit test either).
//
// `documents`/`diagnostics_published_nonempty` are LspServer::Run()'s own
// per-connection local state (see LspServer.cpp), reset on every
// Run() call -- so this test drives two separate RunFrames() calls
// (two separate connections against the SAME `include_graph` instance)
// rather than one, with the file edit + FileWatcher wait happening
// between them. This is a deliberate adaptation, not an invented new
// pattern: phase 1's didOpen and phase 2's didChange each independently
// exercise ComputeDiagnostics against whatever `include_graph` currently
// contains at the time, which is exactly the behavior under test (the
// bug was about `include_graph`'s OWN staleness, not about LspServer's
// per-connection state).
AISTUDIO_TEST(LspServer_PublishDiagnostics_FileWatcherRefreshesIncludeGraph_ClearsStaleUnresolvedInclude) {
    TempProject project;
    const std::string broken_content = "#include \"does_not_exist.hpp\"\nint x = 1;\n";
    project.WriteFile("caller.cpp", broken_content);

    IncludeGraph include_graph;
    AISTUDIO_EXPECT(include_graph.Build(project.RootString()));
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    // Mirrors RunLspMode()'s own FileWatcher wiring (Core/src/main.cpp)
    // exactly: subscribe "FileChanged", rebuild SymbolIndex/IncludeGraph
    // via UpdateFile()/RemoveFile().
    const std::string project_root = project.RootString();
    FileWatcher watcher(project_root, FileScanner{});
    const auto subscription_id = EventBus::Instance().Subscribe(
        "FileChanged", [&symbol_index, &include_graph, &project_root](const std::any& payload) {
            const auto* change = std::any_cast<FileChangeEvent>(&payload);
            if (change == nullptr) {
                return;
            }
            const auto apply = [&](auto& index) {
                if (change->kind == FileChangeKind::Deleted) {
                    index.RemoveFile(change->path);
                } else {
                    index.UpdateFile(project_root, change->path);
                }
            };
            apply(symbol_index);
            apply(include_graph);
        });

    // Establish the baseline synchronously before Start() -- same pattern
    // test_file_watcher.cpp's own
    // FileWatcher_Start_ReactsToChangeFasterThanPollInterval test uses,
    // so this test measures only how the FileWatcher reacts to a real
    // change, not how long its first-poll baseline scan takes.
    const auto baseline = watcher.PollOnce();
    AISTUDIO_EXPECT(baseline.empty());
    watcher.Start();

    const LspServer server(LspServerOptions{.project_root = project_root, .include_graph = &include_graph});
    const std::string uri = FileUri(project.root / "caller.cpp");

    // Phase 1: open the file with the unresolved #include and confirm the
    // diagnostic fires (same assertions as
    // LspServer_PublishDiagnostics_UnresolvedQuotedInclude_SendsWarningSeverityNotification
    // above).
    const auto responses1 = RunFrames(server, {MakeInitialize(1), MakeDidOpen(uri, broken_content)});
    AISTUDIO_EXPECT(responses1.size() == 2);
    AISTUDIO_EXPECT(responses1[1]["method"] == "textDocument/publishDiagnostics");
    AISTUDIO_EXPECT(responses1[1]["params"]["diagnostics"].size() == 1);
    AISTUDIO_EXPECT(responses1[1]["params"]["diagnostics"][0]["severity"] == 2); // Warning

    // Edit the file on disk to remove the offending #include line --
    // simulating the user fixing it and saving.
    const std::string fixed_content = "int x = 1;\n";
    project.WriteFile("caller.cpp", fixed_content);

    // Wait for the FileWatcher's background thread to detect the change
    // and rebuild include_graph -- polling with a timeout on a real
    // observable side effect (the stale edge disappearing from
    // include_graph itself), same pattern as test_file_watcher.cpp's own
    // FileWatcher_Start_ReactsToChangeFasterThanPollInterval test, rather
    // than a fixed sleep.
    const auto has_stale_edge = [&] {
        for (const auto& edge : include_graph.AllEdges()) {
            if (edge.from_file == "caller.cpp" && !edge.is_system && edge.resolved_path.empty()) {
                return true;
            }
        }
        return false;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (has_stale_edge() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    watcher.Stop();
    EventBus::Instance().Unsubscribe("FileChanged", subscription_id);

    AISTUDIO_EXPECT(!has_stale_edge()); // FileWatcher actually rebuilt include_graph in time

    // Phase 2: a subsequent didOpen/didChange-triggered diagnostics
    // computation (a fresh connection -- see this test's own comment
    // above on why) no longer reports the stale warning, because
    // include_graph itself has been refreshed -- this is the exact fix
    // under test. Before this fix, include_graph would still contain the
    // stale edge forever, and this second RunFrames() call would still
    // observe a 2nd (warning) notification, byte-for-byte like phase 1's.
    const auto responses2 = RunFrames(server, {MakeInitialize(1), MakeDidOpen(uri, fixed_content),
                                                MakeDidChange(uri, fixed_content)});
    // No publishDiagnostics notification at all: didOpen computes empty
    // diagnostics against the now-fresh include_graph (a quiet no-op
    // transition, see PublishDiagnosticsIfChanged's own comment in
    // LspServer.cpp), and didChange's own diagnostics stay empty too, so
    // neither publishes -- only the two initialize/didOpen-adjacent
    // responses this connection actually sent an id for. Matches
    // LspServer_PublishDiagnostics_ResolvedInclude_SendsNoNotification's
    // own "responses.size() == 1" assertion style for the single-message
    // case.
    AISTUDIO_EXPECT(responses2.size() == 1);
    for (const auto& response : responses2) {
        AISTUDIO_EXPECT(response.value("method", std::string()) != "textDocument/publishDiagnostics");
    }
}

AISTUDIO_TEST(LspServer_PublishDiagnostics_FirewallExcludesDeniedFile) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns (same fixture
    // shape as LspServer_Definition_FirewallExcludesDeniedFile above).
    const Sandbox firewall(project.RootString());
    const LspServer server(LspServerOptions{.project_root = project.RootString(), .firewall = &firewall});

    const std::string uri = FileUri(project.root / "my_secret.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "void Foo() {\n"), // would otherwise be a parse-error diagnostic
                                              });
    AISTUDIO_EXPECT(responses.size() == 1); // firewalled -- never even computed, let alone sent
}

// ---------------------------------------------------------------------
// textDocument/rename / textDocument/prepareRename.
// ---------------------------------------------------------------------

AISTUDIO_TEST(LspServer_Initialize_WithSymbolIndexAndEnableRename_AdvertisesRenameProviderWithPrepareSupport) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(response["result"]["capabilities"]["renameProvider"]["prepareProvider"] == true);
}

AISTUDIO_TEST(LspServer_Initialize_RenameDisabledByDefault_DoesNotAdvertiseRenameProvider) {
    TempProject project;
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    // enable_rename left at its default (false) -- mirrors
    // enable_git_write_commands' own opt-in-by-default-off convention.
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(!response["result"]["capabilities"].contains("renameProvider"));
}

AISTUDIO_TEST(LspServer_Initialize_EnableRenameButNoSymbolIndex_DoesNotAdvertiseRenameProvider) {
    const LspServer server(LspServerOptions{.enable_rename = true});
    const auto response = RunOne(server, MakeInitialize(1));
    AISTUDIO_EXPECT(!response["result"]["capabilities"].contains("renameProvider"));
}

AISTUDIO_TEST(LspServer_Rename_ToggleDisabled_IsRejectedWithMethodNotFoundError) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    // enable_rename left at its default (false) -- must not be offered,
    // and a client that calls it anyway must be explicitly rejected, not
    // silently given a misleading `null` ("nothing renameable here").
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeRenameRequest(2, uri, 0, 9, "Renamed"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32601);
}

AISTUDIO_TEST(LspServer_Rename_NoSymbolIndexConfigured_IsRejectedWithMethodNotFoundErrorEvenWithToggleOn) {
    // enable_rename alone isn't enough -- same "needs symbol_index AND
    // project_root" gate every other capability in this file already has.
    const LspServer server(LspServerOptions{.enable_rename = true});
    const std::string uri = FileUri(fs::temp_directory_path() / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeRenameRequest(2, uri, 0, 9, "Renamed"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32601);
}

AISTUDIO_TEST(LspServer_Rename_RenamesAllWholeWordOccurrencesWithinTheOneOpenFile) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n"); // makes "Total" a real declared symbol (precision safeguard)
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    // Column layout (0-based): line0 "Total" at col4; line1 "Total" at
    // col8 and col16.
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int Total = 1;\nint y = Total + Total;\n"),
                                                  MakeRenameRequest(2, uri, 0, 5, "Renamed"), // cursor inside "Total"
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result.contains("changes"));
    AISTUDIO_EXPECT(result["changes"].size() == 1); // exactly one file named -- single-file scope
    AISTUDIO_EXPECT(result["changes"].contains(uri));
    const auto& edits = result["changes"][uri];
    AISTUDIO_EXPECT(edits.is_array());
    AISTUDIO_EXPECT(edits.size() == 3);
    for (const auto& edit : edits) {
        AISTUDIO_EXPECT(edit["newText"] == "Renamed");
    }
}

AISTUDIO_TEST(LspServer_Rename_DoesNotTouchOccurrencesInOtherFiles) {
    // Single-file scope's own regression test: a same-named occurrence in
    // a DIFFERENT file must never appear in the WorkspaceEdit, unlike
    // textDocument/references' deliberately project-wide FileScanner scan.
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    project.WriteFile("bar.cpp", "void Bar() {\n    Total = 5;\n}\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeRenameRequest(2, uri, 0, 9, "Renamed"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result["changes"].size() == 1);
    AISTUDIO_EXPECT(result["changes"][uri].size() == 1); // only caller.cpp's own occurrence -- bar.cpp's is never touched
}

AISTUDIO_TEST(LspServer_Rename_DoesNotMatchPartialWordSubstring) {
    TempProject project;
    project.WriteFile("foo.cpp", "int ComputeAll = 0;\nint Compute = 1;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int y = Compute;\nint z = ComputeAll;\n"),
                                                  MakeRenameRequest(2, uri, 0, 9, "Renamed"), // cursor inside "Compute"
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& edits = responses[1]["result"]["changes"][uri];
    AISTUDIO_EXPECT(edits.size() == 1); // NOT the "Compute" substring inside "ComputeAll" on line1
    AISTUDIO_EXPECT(edits[0]["range"]["start"]["line"] == 0);
}

AISTUDIO_TEST(LspServer_Rename_EmptyNewName_IsRejectedWithInvalidParamsError) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeRenameRequest(2, uri, 0, 9, ""),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32602);
}

AISTUDIO_TEST(LspServer_Rename_NewNameWithInvalidCharacters_IsRejectedWithInvalidParamsError) {
    // Adversarial input (this task's own analogue to the GitBackend
    // argument-injection review, docs/ROADMAP.md): "::" is not part of
    // any valid C++ identifier, so this must be rejected outright rather
    // than silently producing a WorkspaceEdit whose newText the client
    // would apply as unusable/invalid C++.
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeRenameRequest(2, uri, 0, 9, "Foo::Bar"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32602);
}

AISTUDIO_TEST(LspServer_Rename_NewNameStartingWithDigit_IsRejectedWithInvalidParamsError) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakeRenameRequest(2, uri, 0, 9, "3Total"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32602);
}

AISTUDIO_TEST(LspServer_Rename_MissingNewNameField_IsRejectedWithInvalidParamsError) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const Json request{
        {"jsonrpc", "2.0"},
        {"id", 2},
        {"method", "textDocument/rename"},
        {"params", Json{{"textDocument", Json{{"uri", uri}}}, {"position", Json{{"line", 0}, {"character", 9}}}}},
    };
    const auto responses =
        RunFrames(server, {MakeInitialize(1), MakeDidOpen(uri, "int x = Total;\n"), request});
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32602);
}

AISTUDIO_TEST(LspServer_Rename_FileOutsideSandbox_ReturnsNull) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns (same fixture
    // shape as LspServer_References_FirewallExcludesDeniedFile above) --
    // the requested document ITSELF is the denied file here (unlike
    // References, where the denied file was merely one of several
    // occurrence sites), matching Rename's single-file scope: the ONE
    // file this request could ever touch is firewalled.
    project.WriteFile("my_secret.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const Sandbox firewall(project.RootString());
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index,
                                             .project_root = project.RootString(),
                                             .firewall = &firewall,
                                             .enable_rename = true});

    const std::string uri = FileUri(project.root / "my_secret.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int Total = 0;\n"),
                                                  MakeRenameRequest(2, uri, 0, 5, "Renamed"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null()); // firewalled -- never even computed
}

AISTUDIO_TEST(LspServer_Rename_UnknownIdentifier_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = NoSuchSymbolAnywhere();\n"),
                                                  MakeRenameRequest(2, uri, 0, 10, "Renamed"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_Rename_NoIdentifierAtCursorPosition_ReturnsNull) {
    // "a rename targeting an identifier that doesn't actually exist at
    // the given position" -- the cursor sits on whitespace with nothing
    // identifier-shaped before it.
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "   \n"),
                                                  MakeRenameRequest(2, uri, 0, 1, "Renamed"),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_Rename_UnopenedDocument_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const auto responses =
        RunFrames(server, {
                               MakeInitialize(1),
                               MakeRenameRequest(2, FileUri(project.root / "never_opened.cpp"), 0, 4, "Renamed"),
                           });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}

AISTUDIO_TEST(LspServer_Rename_SameFileDifferentScopesShareSpelling_KnownFalsePositiveLimitation) {
    // Documents the accepted false-positive limitation
    // LspServerOptions::enable_rename's own comment describes: the
    // precision safeguard only checks "is `x` a real declared symbol
    // SOMEWHERE" (yes -- the global `x` is), then rewrites EVERY
    // whole-word text occurrence of "x" within the one file regardless of
    // scope -- so renaming the LOCAL variable `x` inside Foo() also
    // rewrites the unrelated GLOBAL `x`, even though they are different
    // symbols that merely happen to share a spelling in the same file.
    TempProject project;
    const std::string content = "int x = 0;\nvoid Foo() {\n    int x = 1;\n}\n";
    project.WriteFile("caller.cpp", content);
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, content),
                                                  MakeRenameRequest(2, uri, 2, 8, "y"), // cursor on the LOCAL `x`
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& edits = responses[1]["result"]["changes"][uri];
    // Both the global `x` (line0) and the unrelated local `x` (line2) are
    // rewritten -- the documented limitation, not a bug.
    AISTUDIO_EXPECT(edits.size() == 2);
}

AISTUDIO_TEST(LspServer_PrepareRename_ReturnsRangeAndPlaceholder) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakePrepareRenameRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    const auto& result = responses[1]["result"];
    AISTUDIO_EXPECT(result["placeholder"] == "Total");
    AISTUDIO_EXPECT(result["range"]["start"]["line"] == 0);
    AISTUDIO_EXPECT(result["range"]["start"]["character"] == 8);
    AISTUDIO_EXPECT(result["range"]["end"]["character"] == 13);
}

AISTUDIO_TEST(LspServer_PrepareRename_ToggleDisabled_IsRejectedWithMethodNotFoundError) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString()});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = Total;\n"),
                                                  MakePrepareRenameRequest(2, uri, 0, 9),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["error"]["code"] == -32601);
}

AISTUDIO_TEST(LspServer_PrepareRename_UnknownIdentifier_ReturnsNull) {
    TempProject project;
    project.WriteFile("foo.cpp", "int Total = 0;\n");
    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const LspServer server(
        LspServerOptions{.symbol_index = &symbol_index, .project_root = project.RootString(), .enable_rename = true});
    const std::string uri = FileUri(project.root / "caller.cpp");
    const auto responses = RunFrames(server, {
                                                  MakeInitialize(1),
                                                  MakeDidOpen(uri, "int x = NoSuchSymbolAnywhere();\n"),
                                                  MakePrepareRenameRequest(2, uri, 0, 10),
                                              });
    AISTUDIO_EXPECT(responses.size() == 2);
    AISTUDIO_EXPECT(responses[1]["result"].is_null());
}
