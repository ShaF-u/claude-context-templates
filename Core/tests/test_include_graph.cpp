#include "test_framework.hpp"
#include "Core/Index/IncludeGraph.hpp"

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
        root = fs::temp_directory_path() / fs::path("aistudio_include_test_" + std::to_string(counter++));
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

bool ContainsResolved(const std::vector<std::string>& paths, const std::string& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

} // namespace

AISTUDIO_TEST(IncludeGraph_Build_ResolvesQuotedIncludeToProjectFile) {
    TempProject project;
    project.WriteFile("include/Foo/Bar.hpp", "struct Bar {};\n");
    project.WriteFile("main.cpp", "#include \"Foo/Bar.hpp\"\n");

    IncludeGraph graph;
    AISTUDIO_EXPECT(graph.Build(project.RootString()));

    AISTUDIO_EXPECT(ContainsResolved(graph.Includes("main.cpp"), "include/Foo/Bar.hpp"));
}

AISTUDIO_TEST(IncludeGraph_IncludedBy_IsReverseOfIncludes) {
    TempProject project;
    project.WriteFile("include/Foo/Bar.hpp", "struct Bar {};\n");
    project.WriteFile("main.cpp", "#include \"Foo/Bar.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(ContainsResolved(graph.IncludedBy("include/Foo/Bar.hpp"), "main.cpp"));
}

AISTUDIO_TEST(IncludeGraph_SystemInclude_IsNeverResolved) {
    TempProject project;
    project.WriteFile("main.cpp", "#include <string>\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto edges = graph.AllEdges();
    AISTUDIO_EXPECT(edges.size() == 1);
    AISTUDIO_EXPECT(edges.front().is_system);
    AISTUDIO_EXPECT(edges.front().resolved_path.empty());
}

AISTUDIO_TEST(IncludeGraph_UnknownQuotedInclude_IsUnresolved) {
    TempProject project;
    project.WriteFile("main.cpp", "#include \"DoesNotExist.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto edges = graph.AllEdges();
    AISTUDIO_EXPECT(edges.size() == 1);
    AISTUDIO_EXPECT(edges.front().resolved_path.empty());
}

AISTUDIO_TEST(IncludeGraph_Build_SkipsNonSourceFiles) {
    TempProject project;
    project.WriteFile("README.md", "#include \"Foo.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(graph.Size() == 0);
}

AISTUDIO_TEST(IncludeGraph_Build_MissingRoot_ReturnsError) {
    IncludeGraph graph;
    const auto result = graph.Build((fs::temp_directory_path() / "aistudio_include_does_not_exist").string());
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(IncludeGraph_Build_RebuildOfUnchangedFile_IsCacheHit) {
    TempProject project;
    project.WriteFile("main.cpp", "#include <string>\n");

    IncludeGraph graph;
    graph.Build(project.RootString()); // first build: cold, all misses
    AISTUDIO_EXPECT(graph.Stats().hits == 0);

    graph.Build(project.RootString()); // rebuild, file unchanged
    AISTUDIO_EXPECT(graph.Stats().hits == 1);
}

AISTUDIO_TEST(IncludeGraph_Build_RebuildAfterFileChange_ReExtracts) {
    TempProject project;
    project.WriteFile("include/A.hpp", "\n");
    project.WriteFile("include/B.hpp", "\n");
    project.WriteFile("main.cpp", "#include \"A.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(ContainsResolved(graph.Includes("main.cpp"), "include/A.hpp"));

    project.WriteFile("main.cpp", "#include \"B.hpp\"\n");
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(ContainsResolved(graph.Includes("main.cpp"), "include/B.hpp"));
    AISTUDIO_EXPECT(!ContainsResolved(graph.Includes("main.cpp"), "include/A.hpp"));
}

AISTUDIO_TEST(IncludeGraph_Resolve_PrefersShortestMatchingPath) {
    TempProject project;
    // Two files both end with "Bar.hpp" — the shorter path is the more
    // specific/direct match and should win.
    project.WriteFile("Bar.hpp", "\n");
    project.WriteFile("nested/deeper/Bar.hpp", "\n");
    project.WriteFile("main.cpp", "#include \"Bar.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto includes = graph.Includes("main.cpp");
    AISTUDIO_EXPECT(includes.size() == 1);
    AISTUDIO_EXPECT(includes.front() == "Bar.hpp");
}

AISTUDIO_TEST(IncludeGraph_UpdateFile_AddsNewFilesEdgesWithoutFullRescan) {
    TempProject project;
    project.WriteFile("a.hpp", "\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("b.cpp", "#include \"a.hpp\"\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "b.cpp"));

    const auto includes = graph.Includes("b.cpp");
    AISTUDIO_EXPECT(includes.size() == 1);
    AISTUDIO_EXPECT(includes.front() == "a.hpp");
}

AISTUDIO_TEST(IncludeGraph_UpdateFile_ReplacesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "\n");
    project.WriteFile("b.hpp", "\n");
    project.WriteFile("x.cpp", "#include \"a.hpp\"\n");
    project.WriteFile("y.cpp", "#include \"b.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    project.WriteFile("x.cpp", "#include \"b.hpp\"\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "x.cpp"));

    AISTUDIO_EXPECT(graph.Includes("x.cpp").front() == "b.hpp");
    AISTUDIO_EXPECT(graph.Includes("y.cpp").front() == "b.hpp"); // untouched file survives
}

AISTUDIO_TEST(IncludeGraph_UpdateFile_NewFileCanResolveOtherFilesPreviouslyUnresolvedInclude) {
    // The key behavior distinguishing IncludeGraph from SymbolIndex's own
    // UpdateFile(): resolving one edge depends on the whole project's
    // file list, so adding an unrelated file can retroactively resolve
    // some OTHER file's edge that Build() originally left unresolved.
    TempProject project;
    project.WriteFile("a.cpp", "#include \"newly_added.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(graph.Includes("a.cpp").empty()); // unresolved at first

    project.WriteFile("newly_added.hpp", "\n");
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "newly_added.hpp"));

    const auto includes = graph.Includes("a.cpp");
    AISTUDIO_EXPECT(includes.size() == 1);
    AISTUDIO_EXPECT(includes.front() == "newly_added.hpp");
}

AISTUDIO_TEST(IncludeGraph_UpdateFile_MissingFile_ReturnsError) {
    TempProject project;
    IncludeGraph graph;
    AISTUDIO_EXPECT(graph.UpdateFile(project.RootString(), "does_not_exist.cpp").IsError());
}

AISTUDIO_TEST(IncludeGraph_RemoveFile_RemovesOnlyThatFilesEdges) {
    TempProject project;
    project.WriteFile("a.hpp", "\n");
    project.WriteFile("x.cpp", "#include \"a.hpp\"\n");
    project.WriteFile("y.cpp", "#include \"a.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("x.cpp"));

    AISTUDIO_EXPECT(graph.Includes("x.cpp").empty());
    AISTUDIO_EXPECT(graph.Includes("y.cpp").size() == 1); // untouched
}

AISTUDIO_TEST(IncludeGraph_RemoveFile_UnresolvesEdgesThatPointedAtIt) {
    TempProject project;
    project.WriteFile("a.hpp", "\n");
    project.WriteFile("main.cpp", "#include \"a.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());
    AISTUDIO_EXPECT(graph.Includes("main.cpp").size() == 1);

    AISTUDIO_EXPECT(graph.RemoveFile("a.hpp"));

    AISTUDIO_EXPECT(graph.Includes("main.cpp").empty());
}

AISTUDIO_TEST(IncludeGraph_RemoveFile_UnknownFile_IsNoOp) {
    TempProject project;
    project.WriteFile("a.hpp", "\n");
    project.WriteFile("main.cpp", "#include \"a.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    AISTUDIO_EXPECT(graph.RemoveFile("never_indexed.cpp"));
    AISTUDIO_EXPECT(graph.Includes("main.cpp").size() == 1);
}
