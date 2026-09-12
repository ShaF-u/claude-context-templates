#include "test_framework.hpp"
#include "Core/Index/AstIndex.hpp"

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
        root = fs::temp_directory_path() / fs::path("aistudio_ast_test_" + std::to_string(counter++));
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

} // namespace

AISTUDIO_TEST(AstIndex_Build_IndexesSourceFiles) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    AstIndex index;
    AISTUDIO_EXPECT(index.Build(project.RootString()));
    AISTUDIO_EXPECT(index.Size() == 1);
}

AISTUDIO_TEST(AstIndex_Get_ReturnsTreeForKnownFile) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    AstIndex index;
    index.Build(project.RootString());

    const auto tree = index.Get("a.hpp");
    AISTUDIO_EXPECT(tree.has_value());
    AISTUDIO_EXPECT(tree->kind == "translation_unit");
}

AISTUDIO_TEST(AstIndex_Get_UnknownFile_ReturnsNullopt) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    AstIndex index;
    index.Build(project.RootString());

    AISTUDIO_EXPECT(!index.Get("does_not_exist.hpp").has_value());
}

AISTUDIO_TEST(AstIndex_Build_SkipsNonSourceFiles) {
    TempProject project;
    project.WriteFile("README.md", "class DoesNotCount {\n};\n");

    AstIndex index;
    AISTUDIO_EXPECT(index.Build(project.RootString()));
    AISTUDIO_EXPECT(index.Size() == 0);
}

AISTUDIO_TEST(AstIndex_Build_MissingRoot_ReturnsError) {
    AstIndex index;
    const auto result = index.Build((fs::temp_directory_path() / "aistudio_ast_does_not_exist").string());
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(AstIndex_Build_RebuildOfUnchangedFile_IsCacheHit) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    AstIndex index;
    index.Build(project.RootString()); // first build: cold, all misses
    AISTUDIO_EXPECT(index.Stats().hits == 0);

    index.Build(project.RootString()); // rebuild, file unchanged
    AISTUDIO_EXPECT(index.Stats().hits == 1);
    AISTUDIO_EXPECT(index.Size() == 1);
}

AISTUDIO_TEST(AstIndex_Build_RebuildAfterFileChange_ReExtracts) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    AstIndex index;
    index.Build(project.RootString());

    project.WriteFile("a.hpp", "class Bar {\n};\n");
    index.Build(project.RootString());

    const auto tree = index.Get("a.hpp");
    AISTUDIO_EXPECT(tree.has_value());

    bool found_bar = false;
    for (const auto& child : tree->children) {
        if (child.kind == "class_specifier") {
            for (const auto& grandchild : child.children) {
                if (grandchild.kind == "type_identifier" && grandchild.text == "Bar") {
                    found_bar = true;
                }
            }
        }
    }
    AISTUDIO_EXPECT(found_bar);
}

AISTUDIO_TEST(AstIndex_Stats_ReflectsCacheEntryCount) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");
    project.WriteFile("b.hpp", "class Bar {\n};\n");

    AstIndex index;
    index.Build(project.RootString());
    AISTUDIO_EXPECT(index.Stats().size == 2);
}

AISTUDIO_TEST(AstIndex_UpdateFile_AddsNewFileWithoutFullRescan) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    AstIndex index;
    index.Build(project.RootString());
    AISTUDIO_EXPECT(index.Size() == 1);

    project.WriteFile("b.hpp", "class Bar {\n};\n");
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "b.hpp"));

    AISTUDIO_EXPECT(index.Size() == 2);
    AISTUDIO_EXPECT(index.Get("a.hpp").has_value());
    AISTUDIO_EXPECT(index.Get("b.hpp").has_value());
}

AISTUDIO_TEST(AstIndex_UpdateFile_ReplacesOnlyThatFilesTree) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");
    project.WriteFile("b.hpp", "class Bar {\n};\n");

    AstIndex index;
    index.Build(project.RootString());

    project.WriteFile("a.hpp", "class Renamed {\n};\n");
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "a.hpp"));

    AISTUDIO_EXPECT(index.Size() == 2); // still exactly 2 trees
    AISTUDIO_EXPECT(index.Get("a.hpp").has_value());
    AISTUDIO_EXPECT(index.Get("b.hpp").has_value()); // untouched file survives
}

AISTUDIO_TEST(AstIndex_UpdateFile_NonSourceFile_IsNoOp) {
    TempProject project;
    project.WriteFile("README.md", "class DoesNotCount {\n};\n");

    AstIndex index;
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "README.md"));
    AISTUDIO_EXPECT(index.Size() == 0);
}

AISTUDIO_TEST(AstIndex_UpdateFile_MissingFile_ReturnsError) {
    TempProject project;
    AstIndex index;
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "does_not_exist.hpp").IsError());
}

AISTUDIO_TEST(AstIndex_RemoveFile_RemovesOnlyThatFilesTree) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");
    project.WriteFile("b.hpp", "class Bar {\n};\n");

    AstIndex index;
    index.Build(project.RootString());

    AISTUDIO_EXPECT(index.RemoveFile("a.hpp"));

    AISTUDIO_EXPECT(index.Size() == 1);
    AISTUDIO_EXPECT(!index.Get("a.hpp").has_value());
    AISTUDIO_EXPECT(index.Get("b.hpp").has_value());
}

AISTUDIO_TEST(AstIndex_RemoveFile_UnknownFile_IsNoOp) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    AstIndex index;
    index.Build(project.RootString());

    AISTUDIO_EXPECT(index.RemoveFile("never_indexed.hpp"));
    AISTUDIO_EXPECT(index.Size() == 1);
}
