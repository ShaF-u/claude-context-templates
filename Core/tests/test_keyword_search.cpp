#include "test_framework.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Search/KeywordSearch.hpp"
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
        root = fs::temp_directory_path() / fs::path("aistudio_keyword_search_test_" + std::to_string(counter++));
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

bool ContainsFile(const std::vector<KeywordMatch>& matches, const std::string& file) {
    return std::any_of(matches.begin(), matches.end(), [&](const KeywordMatch& m) { return m.file_path == file; });
}

} // namespace

AISTUDIO_TEST(KeywordSearch_Search_FindsMatchingLine) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n    DoAttack();\n}\n");

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "attack");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().size() == 1);
    AISTUDIO_EXPECT(result.Value().front().line == 2);
    AISTUDIO_EXPECT(result.Value().front().text == "DoAttack();");
}

AISTUDIO_TEST(KeywordSearch_Search_NonAsciiFileName_DoesNotThrow) {
    // Regression test for the same class of bug already fixed elsewhere
    // (see docs/ROADMAP.md non-ASCII path crash sweeps): Search()
    // narrow-constructed root_path and rejoined it with each scanned
    // relative path via fs::path's '/' operator on a std::string, both
    // CP_ACP-unsafe on Windows for non-ASCII input.
    //
    // Written directly via Utf8ToPath rather than TempProject::WriteFile()
    // -- that helper's own root / relative_path append has this same bug
    // for a non-ASCII relative_path.
    TempProject project;
    const std::string relative = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.cpp"; // 日本語.cpp
    {
        std::ofstream out(Utf8ToPath(project.RootString() + "/" + relative), std::ios::binary);
        out << "void Attack() {\n}\n";
    }

    const KeywordSearch search;
    bool threw = false;
    Result<std::vector<KeywordMatch>> result = Result<std::vector<KeywordMatch>>::Ok({});
    try {
        result = search.Search(project.RootString(), "attack");
    } catch (const std::exception&) {
        threw = true;
    }
    AISTUDIO_EXPECT(!threw);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(ContainsFile(result.Value(), relative));
}

AISTUDIO_TEST(KeywordSearch_Search_IsCaseInsensitive) {
    TempProject project;
    project.WriteFile("a.cpp", "void Attack() {\n}\n");

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "ATTACK");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(!result.Value().empty());
}

AISTUDIO_TEST(KeywordSearch_Search_MatchesAcrossMultipleFiles) {
    TempProject project;
    project.WriteFile("a.cpp", "// attack logic\n");
    project.WriteFile("b.cpp", "int attack = 1;\n");

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "attack");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().size() == 2);
    AISTUDIO_EXPECT(ContainsFile(result.Value(), "a.cpp"));
    AISTUDIO_EXPECT(ContainsFile(result.Value(), "b.cpp"));
}

// docs/ROADMAP.md CE-5: a "line" (0x0A-delimited) read out of a binary
// file can contain the query alongside bytes that aren't valid UTF-8 --
// this used to crash the whole response at JSON serialization instead of
// just not matching that line.
AISTUDIO_TEST(KeywordSearch_Search_BinaryLineContainingQuery_IsSkippedNotCrashed) {
    TempProject project;
    std::string content = "normal line\n";
    content += "findme";
    content += '\xFA';
    content += '\x17';
    content += "more binary bytes\n";
    project.WriteFile("asset.bin", content);

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "findme");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().empty());
}

AISTUDIO_TEST(KeywordSearch_Search_NoMatches_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "doesnotexist");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().empty());
}

AISTUDIO_TEST(KeywordSearch_Search_EmptyQuery_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().empty());
}

AISTUDIO_TEST(KeywordSearch_Search_MissingRoot_ReturnsError) {
    const KeywordSearch search;
    const auto result = search.Search((fs::temp_directory_path() / "aistudio_keyword_does_not_exist").string(), "attack");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(KeywordSearch_Search_RanksWholeWordAboveSubstring) {
    TempProject project;
    project.WriteFile("a.cpp", "int attacker = 1;\n"); // substring only
    project.WriteFile("b.cpp", "int attack = 1;\n");    // whole word

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "attack");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().size() == 2);
    AISTUDIO_EXPECT(result.Value().front().file_path == "b.cpp");
}

AISTUDIO_TEST(KeywordSearch_Search_RanksMoreOccurrencesHigher) {
    TempProject project;
    project.WriteFile("a.cpp", "int attack = 1;\n");
    project.WriteFile("b.cpp", "int attack = attack + attack;\n");

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "attack");
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().front().file_path == "b.cpp");
}

AISTUDIO_TEST(KeywordSearch_Search_RespectsMaxResults) {
    TempProject project;
    std::string content;
    for (int i = 0; i < 10; ++i) {
        content += "attack line\n";
    }
    project.WriteFile("a.cpp", content);

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "attack", 3);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().size() == 3);
}

// --- Scanned-file overload (ContextRetriever's single-scan reuse) ---

AISTUDIO_TEST(KeywordSearch_ScannedOverload_ReadsFromCacheNotDisk) {
    TempProject project;
    // Metadata for a path that does not exist on disk -- if a match is
    // found, it can only have come from the cache, never a fallback read.
    FileMetadata metadata;
    metadata.path = "ghost.cpp";
    metadata.content_hash = HashContent("void DoAttack() {}\n");
    metadata.size = 20;

    FileCache cache;
    cache.Put(metadata, "void DoAttack() {}\n");

    const KeywordSearch search;
    const auto result = search.Search({metadata}, project.RootString(), "DoAttack", 200, &cache);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(ContainsFile(result.Value(), "ghost.cpp"));
}

AISTUDIO_TEST(KeywordSearch_ScannedOverload_CacheMiss_FallsBackToDisk) {
    TempProject project;
    project.WriteFile("a.cpp", "void DoAttack() {}\n");

    FileMetadata metadata;
    metadata.path = "a.cpp";
    metadata.content_hash = HashContent("void DoAttack() {}\n");
    metadata.size = 20;

    FileCache empty_cache; // nothing Put() -- forces the disk fallback
    const KeywordSearch search;
    const auto result = search.Search({metadata}, project.RootString(), "DoAttack", 200, &empty_cache);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(ContainsFile(result.Value(), "a.cpp"));
}

AISTUDIO_TEST(KeywordSearch_ScannedOverload_NoContentCache_ReadsDisk) {
    TempProject project;
    project.WriteFile("a.cpp", "void DoAttack() {}\n");

    FileMetadata metadata;
    metadata.path = "a.cpp";

    const KeywordSearch search;
    const auto result = search.Search({metadata}, project.RootString(), "DoAttack", 200);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(ContainsFile(result.Value(), "a.cpp"));
}

AISTUDIO_TEST(KeywordSearch_ScannedOverload_EmptyQuery_ReturnsEmpty) {
    FileMetadata metadata;
    metadata.path = "a.cpp";
    const KeywordSearch search;
    const auto result = search.Search({metadata}, ".", "", 200);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().empty());
}

AISTUDIO_TEST(KeywordSearch_RootOverload_StillWorksUnchanged) {
    TempProject project;
    project.WriteFile("a.cpp", "void DoAttack() {}\n");

    const KeywordSearch search;
    const auto result = search.Search(project.RootString(), "DoAttack");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(ContainsFile(result.Value(), "a.cpp"));
}
