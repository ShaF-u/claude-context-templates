#include "test_framework.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/Util/Utf8.hpp"

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
        root = fs::temp_directory_path() / fs::path("aistudio_symbol_test_" + std::to_string(counter++));
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

bool Contains(const std::vector<Symbol>& symbols, const std::string& name) {
    return std::any_of(symbols.begin(), symbols.end(), [&](const Symbol& s) { return s.name == name; });
}

} // namespace

AISTUDIO_TEST(SymbolIndex_Build_IndexesSourceFiles) {
    TempProject project;
    project.WriteFile("src/Player.hpp", "class Player {\npublic:\n    void Attack();\n};\n");
    project.WriteFile("src/Player.cpp", "void Player::Attack() {\n}\n");

    SymbolIndex index;
    AISTUDIO_EXPECT(index.Build(project.RootString()));

    const auto symbols = index.All();
    AISTUDIO_EXPECT(Contains(symbols, "Player"));
    AISTUDIO_EXPECT(Contains(symbols, "Player::Attack"));
}

AISTUDIO_TEST(SymbolIndex_Build_NonAsciiDirectoryName_DoesNotThrow) {
    // Regression test for the same class of bug already fixed in
    // FileScanner::Scan()/WorkspaceHash()/Sandbox::Check() (see
    // docs/ROADMAP.md): Build() narrow-constructed root_path and rejoined
    // it with each scanned relative path via fs::path's '/' operator on a
    // std::string, both CP_ACP-unsafe on Windows for non-ASCII input.
    //
    // Written directly via Utf8ToPath rather than TempProject::WriteFile()
    // -- that helper's own root / relative_path append has this same bug
    // for a non-ASCII relative_path.
    TempProject project;
    const std::string relative_dir = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e"; // 日本語
    const fs::path dir = Utf8ToPath(project.RootString()) / Utf8ToPath(relative_dir);
    fs::create_directories(dir);
    {
        std::ofstream out(dir / "Player.cpp", std::ios::binary);
        out << "class Player {\npublic:\n    void Attack();\n};\n";
    }

    SymbolIndex index;
    bool threw = false;
    Result<void> build_result = Result<void>::Ok();
    try {
        build_result = index.Build(project.RootString());
    } catch (const std::exception&) {
        threw = true;
    }
    AISTUDIO_EXPECT(!threw);
    AISTUDIO_EXPECT(build_result.IsOk());
    AISTUDIO_EXPECT(Contains(index.All(), "Player"));
}

AISTUDIO_TEST(SymbolIndex_Build_SkipsNonSourceFiles) {
    TempProject project;
    project.WriteFile("README.md", "class DoesNotCount {\n};\n");

    SymbolIndex index;
    AISTUDIO_EXPECT(index.Build(project.RootString()));
    AISTUDIO_EXPECT(index.Size() == 0);
}

AISTUDIO_TEST(SymbolIndex_Build_RespectsFileScannerIgnoreRules) {
    TempProject project;
    project.WriteFile("build/generated.cpp", "class Generated {\n};\n");

    SymbolIndex index;
    AISTUDIO_EXPECT(index.Build(project.RootString()));
    AISTUDIO_EXPECT(index.Size() == 0);
}

AISTUDIO_TEST(SymbolIndex_FindByName_ReturnsMatchingSymbols) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");
    project.WriteFile("b.hpp", "struct Bar {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const auto found = index.FindByName("Foo");
    AISTUDIO_EXPECT(found.size() == 1);
    AISTUDIO_EXPECT(found.front().kind == SymbolKind::Class);
}

AISTUDIO_TEST(SymbolIndex_Build_MissingRoot_ReturnsError) {
    SymbolIndex index;
    const auto result = index.Build((fs::temp_directory_path() / "aistudio_symbol_does_not_exist").string());
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(SymbolIndex_Build_ReplacesPreviousResults) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());
    AISTUDIO_EXPECT(index.Size() == 1);

    project.WriteFile("b.hpp", "class Bar {\n};\n");
    index.Build(project.RootString());
    AISTUDIO_EXPECT(index.Size() == 2);
}

AISTUDIO_TEST(SymbolIndex_Build_RebuildOfUnchangedFile_IsCacheHit) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString()); // first build: cold, all misses
    AISTUDIO_EXPECT(index.Stats().hits == 0);

    index.Build(project.RootString()); // rebuild, file unchanged
    AISTUDIO_EXPECT(index.Stats().hits == 1);
    AISTUDIO_EXPECT(index.Size() == 1);
    AISTUDIO_EXPECT(Contains(index.All(), "Foo"));
}

AISTUDIO_TEST(SymbolIndex_Build_RebuildAfterFileChange_ReExtracts) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());
    AISTUDIO_EXPECT(Contains(index.All(), "Foo"));
    AISTUDIO_EXPECT(!Contains(index.All(), "Bar"));

    project.WriteFile("a.hpp", "class Bar {\n};\n");
    index.Build(project.RootString());
    AISTUDIO_EXPECT(Contains(index.All(), "Bar"));
    AISTUDIO_EXPECT(!Contains(index.All(), "Foo"));
}

AISTUDIO_TEST(SymbolIndex_Stats_ReflectsCacheEntryCount) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");
    project.WriteFile("b.hpp", "class Bar {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());
    AISTUDIO_EXPECT(index.Stats().size == 2);
}

AISTUDIO_TEST(SymbolIndex_UpdateFile_AddsNewFileWithoutFullRescan) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());
    AISTUDIO_EXPECT(index.Size() == 1);

    // Written after Build() -- a full Build() would pick this up too, but
    // UpdateFile() must add it without one.
    project.WriteFile("b.hpp", "class Bar {\n};\n");
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "b.hpp"));

    AISTUDIO_EXPECT(index.Size() == 2);
    AISTUDIO_EXPECT(Contains(index.All(), "Foo"));
    AISTUDIO_EXPECT(Contains(index.All(), "Bar"));
}

AISTUDIO_TEST(SymbolIndex_UpdateFile_ReplacesOnlyThatFilesSymbols) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");
    project.WriteFile("b.hpp", "class Bar {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    project.WriteFile("a.hpp", "class FooRenamed {\n};\n");
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "a.hpp"));

    const auto symbols = index.All();
    AISTUDIO_EXPECT(symbols.size() == 2); // still exactly 2: FooRenamed + Bar
    AISTUDIO_EXPECT(Contains(symbols, "FooRenamed"));
    AISTUDIO_EXPECT(!Contains(symbols, "Foo"));
    AISTUDIO_EXPECT(Contains(symbols, "Bar")); // untouched file's symbols survive
}

AISTUDIO_TEST(SymbolIndex_UpdateFile_NonSourceFile_IsNoOp) {
    TempProject project;
    project.WriteFile("README.md", "class DoesNotCount {\n};\n");

    SymbolIndex index;
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "README.md"));
    AISTUDIO_EXPECT(index.Size() == 0);
}

AISTUDIO_TEST(SymbolIndex_UpdateFile_MissingFile_ReturnsError) {
    TempProject project;
    SymbolIndex index;
    AISTUDIO_EXPECT(index.UpdateFile(project.RootString(), "does_not_exist.hpp").IsError());
}

AISTUDIO_TEST(SymbolIndex_UpdateFile_RefreshesCacheEntry) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    project.WriteFile("a.hpp", "class Bar {\n};\n");
    index.UpdateFile(project.RootString(), "a.hpp");

    // A subsequent full Build() should cache-hit the freshly updated
    // entry rather than re-extracting again.
    index.Build(project.RootString());
    AISTUDIO_EXPECT(Contains(index.All(), "Bar"));
    AISTUDIO_EXPECT(index.Stats().hits >= 1);
}

AISTUDIO_TEST(SymbolIndex_RemoveFile_RemovesOnlyThatFilesSymbols) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");
    project.WriteFile("b.hpp", "class Bar {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    AISTUDIO_EXPECT(index.RemoveFile("a.hpp"));

    const auto symbols = index.All();
    AISTUDIO_EXPECT(symbols.size() == 1);
    AISTUDIO_EXPECT(Contains(symbols, "Bar"));
    AISTUDIO_EXPECT(!Contains(symbols, "Foo"));
}

AISTUDIO_TEST(SymbolIndex_RemoveFile_UnknownFile_IsNoOp) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    AISTUDIO_EXPECT(index.RemoveFile("never_indexed.hpp"));
    AISTUDIO_EXPECT(index.Size() == 1);
}
