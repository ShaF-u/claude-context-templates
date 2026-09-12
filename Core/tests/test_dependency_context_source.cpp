#include "test_framework.hpp"
#include "Core/Context/DependencyContextSource.hpp"

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
        root = fs::temp_directory_path() / fs::path("aistudio_dep_context_test_" + std::to_string(counter++));
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

bool ContainsId(const std::vector<ContextItem>& items, const std::string& id) {
    return std::any_of(items.begin(), items.end(), [&](const ContextItem& item) { return item.id == id; });
}

} // namespace

AISTUDIO_TEST(MakeDependencyContextItems_Includes_OneItemPerResolvedDependency) {
    TempProject project;
    project.WriteFile("include/Foo/Bar.hpp", "struct Bar {};\n");
    project.WriteFile("main.cpp", "#include \"Foo/Bar.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes);
    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items.front().id == "main.cpp->include/Foo/Bar.hpp");
    AISTUDIO_EXPECT(items.front().content == "main.cpp includes include/Foo/Bar.hpp");
}

AISTUDIO_TEST(MakeDependencyContextItems_SetsSourceAndCompressionLevel) {
    TempProject project;
    project.WriteFile("include/Foo/Bar.hpp", "struct Bar {};\n");
    project.WriteFile("main.cpp", "#include \"Foo/Bar.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes);
    AISTUDIO_EXPECT(items.front().source == ContextSourceKind::Dependency);
    AISTUDIO_EXPECT(items.front().compression == CompressionLevel::Reference);
    AISTUDIO_EXPECT(items.front().estimated_tokens > 0);
}

AISTUDIO_TEST(MakeDependencyContextItems_DependsOn_ReferencesTargetFile) {
    TempProject project;
    project.WriteFile("include/Foo/Bar.hpp", "struct Bar {};\n");
    project.WriteFile("main.cpp", "#include \"Foo/Bar.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes);
    AISTUDIO_EXPECT(items.front().depends_on.size() == 1);
    AISTUDIO_EXPECT(items.front().depends_on.front() == "main.cpp");
}

AISTUDIO_TEST(MakeDependencyContextItems_IncludedBy_ReturnsReverseEdges) {
    TempProject project;
    project.WriteFile("include/Foo/Bar.hpp", "struct Bar {};\n");
    project.WriteFile("main.cpp", "#include \"Foo/Bar.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "include/Foo/Bar.hpp", DependencyDirection::IncludedBy);
    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items.front().id == "main.cpp->include/Foo/Bar.hpp");
    AISTUDIO_EXPECT(items.front().content == "main.cpp includes include/Foo/Bar.hpp");
}

AISTUDIO_TEST(MakeDependencyContextItems_NoDependencies_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("main.cpp", "int main() { return 0; }\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes);
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(MakeDependencyContextItems_UnresolvedIncludes_AreExcluded) {
    TempProject project;
    project.WriteFile("main.cpp", "#include <string>\n#include \"DoesNotExist.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes);
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(MakeDependencyContextItems_MultipleDependencies_OneItemEach) {
    TempProject project;
    project.WriteFile("include/A.hpp", "\n");
    project.WriteFile("include/B.hpp", "\n");
    project.WriteFile("main.cpp", "#include \"A.hpp\"\n#include \"B.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes);
    AISTUDIO_EXPECT(items.size() == 2);
    AISTUDIO_EXPECT(ContainsId(items, "main.cpp->include/A.hpp"));
    AISTUDIO_EXPECT(ContainsId(items, "main.cpp->include/B.hpp"));
}

AISTUDIO_TEST(MakeDependencyContextItems_PassesFirewall_ExcludesRejectedRelatedPath) {
    TempProject project;
    project.WriteFile("include/A.hpp", "\n");
    project.WriteFile("include/B.hpp", "\n");
    project.WriteFile("main.cpp", "#include \"A.hpp\"\n#include \"B.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(
        graph, "main.cpp", DependencyDirection::Includes, 40,
        [](const std::string& related_path) { return related_path != "include/A.hpp"; });
    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(ContainsId(items, "main.cpp->include/B.hpp"));
}

AISTUDIO_TEST(MakeDependencyContextItems_NoPassesFirewall_IncludesEverything) {
    TempProject project;
    project.WriteFile("include/A.hpp", "\n");
    project.WriteFile("main.cpp", "#include \"A.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes);
    AISTUDIO_EXPECT(items.size() == 1); // default nullptr predicate -- unchanged pre-existing behavior
}

AISTUDIO_TEST(MakeDependencyContextItems_CustomPriority_IsRespected) {
    TempProject project;
    project.WriteFile("include/Foo/Bar.hpp", "struct Bar {};\n");
    project.WriteFile("main.cpp", "#include \"Foo/Bar.hpp\"\n");

    IncludeGraph graph;
    graph.Build(project.RootString());

    const auto items = MakeDependencyContextItems(graph, "main.cpp", DependencyDirection::Includes, 75);
    AISTUDIO_EXPECT(items.front().priority == 75);
}
