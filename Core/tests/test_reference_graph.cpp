#include "test_framework.hpp"
#include "Core/Index/ReferenceGraph.hpp"

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
        root = fs::temp_directory_path() / fs::path("aistudio_reference_test_" + std::to_string(counter++));
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

bool ContainsFile(const std::vector<ReferenceEdge>& edges, const std::string& file) {
    return std::any_of(edges.begin(), edges.end(), [&](const ReferenceEdge& e) { return e.referencing_file == file; });
}

} // namespace

AISTUDIO_TEST(ReferenceGraph_Build_IndexesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph graph;
    AISTUDIO_EXPECT(graph.Build(project.RootString()));
    AISTUDIO_EXPECT(graph.Size() == 1);
}

AISTUDIO_TEST(ReferenceGraph_References_ReturnsAllSitesAcrossFiles) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");
    project.WriteFile("b.hpp", "void Use(Bar b) {\n}\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());

    const auto refs = graph.References("Bar");
    AISTUDIO_EXPECT(refs.size() == 2);
    AISTUDIO_EXPECT(ContainsFile(refs, "a.hpp"));
    AISTUDIO_EXPECT(ContainsFile(refs, "b.hpp"));
}

AISTUDIO_TEST(ReferenceGraph_Build_SkipsNonSourceFiles) {
    TempProject project;
    project.WriteFile("README.md", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(graph.Size() == 0);
}

AISTUDIO_TEST(ReferenceGraph_Build_MissingRoot_ReturnsError) {
    ReferenceGraph graph;
    const auto result = graph.Build((fs::temp_directory_path() / "aistudio_reference_does_not_exist").string());
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(ReferenceGraph_Build_RebuildOfUnchangedFile_IsCacheHit) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString()); // first build: cold, all misses
    AISTUDIO_EXPECT(graph.Stats().hits == 0);

    graph.Build(project.RootString()); // rebuild, file unchanged
    AISTUDIO_EXPECT(graph.Stats().hits == 1);
}

AISTUDIO_TEST(ReferenceGraph_Build_RebuildAfterFileChange_ReExtracts) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(!graph.References("Bar").empty());

    project.WriteFile("a.hpp", "class Foo {\n    Qux q;\n};\n");
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(graph.References("Bar").empty());
    AISTUDIO_EXPECT(!graph.References("Qux").empty());
}

AISTUDIO_TEST(ReferenceGraph_References_UnknownType_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.References("DoesNotExist").empty());
}

AISTUDIO_TEST(ReferenceGraph_UpdateFile_AddsNewFilesEdgesWithoutFullRescan) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("b.hpp", "class Baz {\n    Qux q;\n};\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "b.hpp"));

    AISTUDIO_EXPECT(!graph.References("Bar").empty());
    AISTUDIO_EXPECT(!graph.References("Qux").empty());
}

AISTUDIO_TEST(ReferenceGraph_UpdateFile_ReplacesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");
    project.WriteFile("b.hpp", "class Baz {\n    Qux q;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("a.hpp", "class Foo {\n    Renamed r;\n};\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "a.hpp"));

    AISTUDIO_EXPECT(!graph.References("Renamed").empty());
    AISTUDIO_EXPECT(graph.References("Bar").empty());
    AISTUDIO_EXPECT(!graph.References("Qux").empty()); // untouched file survives
}

AISTUDIO_TEST(ReferenceGraph_UpdateFile_MissingFile_ReturnsError) {
    TempProject project;
    ReferenceGraph graph;
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "does_not_exist.hpp").IsError());
}

AISTUDIO_TEST(ReferenceGraph_RemoveFile_RemovesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");
    project.WriteFile("b.hpp", "class Baz {\n    Qux q;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("a.hpp"));

    AISTUDIO_EXPECT(graph.References("Bar").empty());
    AISTUDIO_EXPECT(!graph.References("Qux").empty()); // untouched
}

AISTUDIO_TEST(ReferenceGraph_RemoveFile_UnknownFile_IsNoOp) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n    Bar b;\n};\n");

    ReferenceGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("never_indexed.hpp"));
    AISTUDIO_EXPECT(!graph.References("Bar").empty());
}
