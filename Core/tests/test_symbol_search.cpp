#include "test_framework.hpp"
#include "Core/Search/SymbolSearch.hpp"

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
        root = fs::temp_directory_path() / fs::path("aistudio_symbol_search_test_" + std::to_string(counter++));
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

AISTUDIO_TEST(SymbolSearch_Search_ExactMatch_RanksHighest) {
    TempProject project;
    project.WriteFile("a.hpp", "class Attack {\n};\nclass AttackComponent {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "Attack");
    AISTUDIO_EXPECT(matches.size() == 2);
    AISTUDIO_EXPECT(matches.front().symbol.name == "Attack");
    AISTUDIO_EXPECT(matches.front().score > matches.back().score);
}

AISTUDIO_TEST(SymbolSearch_Search_PrefixMatch_IsIncluded) {
    TempProject project;
    project.WriteFile("a.hpp", "class AttackComponent {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "Attack");
    AISTUDIO_EXPECT(matches.size() == 1);
    AISTUDIO_EXPECT(matches.front().symbol.name == "AttackComponent");
}

AISTUDIO_TEST(SymbolSearch_Search_SubstringMatch_IsIncluded) {
    TempProject project;
    project.WriteFile("a.hpp", "class PlayerAttackHandler {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "Attack");
    AISTUDIO_EXPECT(matches.size() == 1);
}

AISTUDIO_TEST(SymbolSearch_Search_MatchesTrailingIdentifierOfQualifiedName) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo::Attack() {\n}\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "Attack");
    AISTUDIO_EXPECT(matches.size() == 1);
    AISTUDIO_EXPECT(matches.front().symbol.name == "Foo::Attack");
}

AISTUDIO_TEST(SymbolSearch_Search_IsCaseInsensitive) {
    TempProject project;
    project.WriteFile("a.hpp", "class Attack {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "attack");
    AISTUDIO_EXPECT(matches.size() == 1);
}

AISTUDIO_TEST(SymbolSearch_Search_NoMatch_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "DoesNotExist");
    AISTUDIO_EXPECT(matches.empty());
}

AISTUDIO_TEST(SymbolSearch_Search_EmptyQuery_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "");
    AISTUDIO_EXPECT(matches.empty());
}

AISTUDIO_TEST(SymbolSearch_Search_RespectsMaxResults) {
    TempProject project;
    project.WriteFile("a.hpp", "class Attack1 {\n};\nclass Attack2 {\n};\nclass Attack3 {\n};\n");

    SymbolIndex index;
    index.Build(project.RootString());

    const SymbolSearch search;
    const auto matches = search.Search(index, "Attack", 2);
    AISTUDIO_EXPECT(matches.size() == 2);
}
