#include "test_framework.hpp"
#include "Core/Index/InheritanceGraph.hpp"

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
        root = fs::temp_directory_path() / fs::path("aistudio_inheritance_test_" + std::to_string(counter++));
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

bool ContainsBase(const std::vector<InheritanceEdge>& edges, const std::string& base_name) {
    return std::any_of(edges.begin(), edges.end(), [&](const InheritanceEdge& e) { return e.base_name == base_name; });
}

bool ContainsDerived(const std::vector<InheritanceEdge>& edges, const std::string& derived_name) {
    return std::any_of(edges.begin(), edges.end(),
                        [&](const InheritanceEdge& e) { return e.derived_name == derived_name; });
}

} // namespace

AISTUDIO_TEST(InheritanceGraph_Build_IndexesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph graph;
    AISTUDIO_EXPECT(graph.Build(project.RootString()));
    AISTUDIO_EXPECT(graph.Size() == 1);
}

AISTUDIO_TEST(InheritanceGraph_Bases_ReturnsDirectBasesOfDerived) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar, private Baz {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());

    const auto bases = graph.Bases("Foo");
    AISTUDIO_EXPECT(bases.size() == 2);
    AISTUDIO_EXPECT(ContainsBase(bases, "Bar"));
    AISTUDIO_EXPECT(ContainsBase(bases, "Baz"));
}

AISTUDIO_TEST(InheritanceGraph_Derived_ReturnsDirectSubclassesOfBase) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\nclass Qux : public Bar {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());

    const auto derived = graph.Derived("Bar");
    AISTUDIO_EXPECT(derived.size() == 2);
    AISTUDIO_EXPECT(ContainsDerived(derived, "Foo"));
    AISTUDIO_EXPECT(ContainsDerived(derived, "Qux"));
}

AISTUDIO_TEST(InheritanceGraph_Build_SkipsNonSourceFiles) {
    TempProject project;
    project.WriteFile("README.md", "class Foo : public Bar {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(graph.Size() == 0);
}

AISTUDIO_TEST(InheritanceGraph_Build_MissingRoot_ReturnsError) {
    InheritanceGraph graph;
    const auto result = graph.Build((fs::temp_directory_path() / "aistudio_inheritance_does_not_exist").string());
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(InheritanceGraph_Build_RebuildOfUnchangedFile_IsCacheHit) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString()); // first build: cold, all misses
    AISTUDIO_EXPECT(graph.Stats().hits == 0);

    graph.Build(project.RootString()); // rebuild, file unchanged
    AISTUDIO_EXPECT(graph.Stats().hits == 1);
}

AISTUDIO_TEST(InheritanceGraph_Build_RebuildAfterFileChange_ReExtracts) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Foo"), "Bar"));

    project.WriteFile("a.hpp", "class Foo : public Qux {\n};\n");
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Foo"), "Qux"));
    AISTUDIO_EXPECT(!ContainsBase(graph.Bases("Foo"), "Bar"));
}

AISTUDIO_TEST(InheritanceGraph_Bases_UnknownDerived_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.Bases("DoesNotExist").empty());
}

AISTUDIO_TEST(InheritanceGraph_UpdateFile_AddsNewFilesEdgesWithoutFullRescan) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("b.hpp", "class Baz : public Qux {\n};\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "b.hpp"));

    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Foo"), "Bar"));
    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Baz"), "Qux"));
}

AISTUDIO_TEST(InheritanceGraph_UpdateFile_ReplacesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");
    project.WriteFile("b.hpp", "class Baz : public Qux {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("a.hpp", "class Foo : public Renamed {\n};\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "a.hpp"));

    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Foo"), "Renamed"));
    AISTUDIO_EXPECT(!ContainsBase(graph.Bases("Foo"), "Bar"));
    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Baz"), "Qux")); // untouched file survives
}

AISTUDIO_TEST(InheritanceGraph_UpdateFile_MissingFile_ReturnsError) {
    TempProject project;
    InheritanceGraph graph;
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "does_not_exist.hpp").IsError());
}

AISTUDIO_TEST(InheritanceGraph_RemoveFile_RemovesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");
    project.WriteFile("b.hpp", "class Baz : public Qux {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("a.hpp"));

    AISTUDIO_EXPECT(graph.Bases("Foo").empty());
    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Baz"), "Qux")); // untouched
}

AISTUDIO_TEST(InheritanceGraph_RemoveFile_UnknownFile_IsNoOp) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo : public Bar {\n};\n");

    InheritanceGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("never_indexed.hpp"));
    AISTUDIO_EXPECT(ContainsBase(graph.Bases("Foo"), "Bar"));
}
