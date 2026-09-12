#include "test_framework.hpp"
#include "Core/Index/CallGraph.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_call_test_" + std::to_string(counter++));
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

bool ContainsCallee(const std::vector<CallEdge>& edges, const std::string& callee_text) {
    return std::any_of(edges.begin(), edges.end(), [&](const CallEdge& e) { return e.callee_text == callee_text; });
}

} // namespace

AISTUDIO_TEST(CallGraph_Build_IndexesCallSites) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");

    CallGraph graph;
    AISTUDIO_EXPECT(graph.Build(project.RootString()));
    AISTUDIO_EXPECT(graph.Size() == 1);
}

AISTUDIO_TEST(CallGraph_Callees_ReturnsCallsMadeByCaller) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n    Baz();\n}\nvoid Other() {\n    Qux();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    const auto callees = graph.Callees("Foo");
    AISTUDIO_EXPECT(callees.size() == 2);
    AISTUDIO_EXPECT(ContainsCallee(callees, "Bar"));
    AISTUDIO_EXPECT(ContainsCallee(callees, "Baz"));
}

AISTUDIO_TEST(CallGraph_Callers_MatchesExactCalleeText) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    const auto callers = graph.Callers("Bar");
    AISTUDIO_EXPECT(callers.size() == 1);
    AISTUDIO_EXPECT(callers.front().caller_name == "Foo");
}

AISTUDIO_TEST(CallGraph_Callers_MatchesTrailingIdentifierOfMemberCall) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    selector.Select();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    // Querying by the plain method name finds a call site written as
    // "selector.Select(...)" even though the raw callee_text differs.
    const auto callers = graph.Callers("Select");
    AISTUDIO_EXPECT(callers.size() == 1);
    AISTUDIO_EXPECT(callers.front().caller_name == "Foo");
}

AISTUDIO_TEST(CallGraph_Build_SkipsNonSourceFiles) {
    TempProject project;
    project.WriteFile("README.md", "void Foo() { Bar(); }\n");

    CallGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(graph.Size() == 0);
}

AISTUDIO_TEST(CallGraph_Build_MissingRoot_ReturnsError) {
    CallGraph graph;
    const auto result = graph.Build((fs::temp_directory_path() / "aistudio_call_does_not_exist").string());
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(CallGraph_Build_RebuildOfUnchangedFile_IsCacheHit) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString()); // first build: cold, all misses
    AISTUDIO_EXPECT(graph.Stats().hits == 0);

    graph.Build(project.RootString()); // rebuild, file unchanged
    AISTUDIO_EXPECT(graph.Stats().hits == 1);
}

AISTUDIO_TEST(CallGraph_Build_RebuildAfterFileChange_ReExtracts) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Foo"), "Bar"));

    project.WriteFile("a.cpp", "void Foo() {\n    Qux();\n}\n");
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Foo"), "Qux"));
    AISTUDIO_EXPECT(!ContainsCallee(graph.Callees("Foo"), "Bar"));
}

AISTUDIO_TEST(CallGraph_Callees_UnknownCaller_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.Callees("DoesNotExist").empty());
}

AISTUDIO_TEST(CallGraph_UpdateFile_AddsNewFilesEdgesWithoutFullRescan) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("b.cpp", "void Baz() {\n    Qux();\n}\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "b.cpp"));

    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Foo"), "Bar"));
    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Baz"), "Qux"));
}

AISTUDIO_TEST(CallGraph_UpdateFile_ReplacesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");
    project.WriteFile("b.cpp", "void Baz() {\n    Qux();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("a.cpp", "void Foo() {\n    Renamed();\n}\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "a.cpp"));

    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Foo"), "Renamed"));
    AISTUDIO_EXPECT(!ContainsCallee(graph.Callees("Foo"), "Bar"));
    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Baz"), "Qux")); // untouched file survives
}

AISTUDIO_TEST(CallGraph_UpdateFile_MissingFile_ReturnsError) {
    TempProject project;
    CallGraph graph;
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "does_not_exist.cpp").IsError());
}

AISTUDIO_TEST(CallGraph_RemoveFile_RemovesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");
    project.WriteFile("b.cpp", "void Baz() {\n    Qux();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("a.cpp"));

    AISTUDIO_EXPECT(graph.Callees("Foo").empty());
    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Baz"), "Qux")); // untouched
}

AISTUDIO_TEST(CallGraph_RemoveFile_UnknownFile_IsNoOp) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    Bar();\n}\n");

    CallGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("never_indexed.cpp"));
    AISTUDIO_EXPECT(ContainsCallee(graph.Callees("Foo"), "Bar"));
}
