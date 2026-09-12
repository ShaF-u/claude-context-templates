#include "test_framework.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

// A throwaway directory tree per test, cleaned up on destruction — real
// filesystem I/O rather than mocking it away, since FileScanner's whole
// job is walking a real directory correctly.
struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_test_" + std::to_string(counter++));
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

bool ContainsPath(const std::vector<FileMetadata>& files, const std::string& path) {
    return std::any_of(files.begin(), files.end(), [&](const FileMetadata& m) { return m.path == path; });
}

} // namespace

AISTUDIO_TEST(FileScanner_Scan_FindsRegularFiles) {
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}");
    project.WriteFile("README.md", "# Hello");

    const FileScanner scanner;
    const auto result = scanner.Scan(project.RootString());
    AISTUDIO_EXPECT(result.IsOk());

    const auto& files = result.Value();
    AISTUDIO_EXPECT(files.size() == 2);
    AISTUDIO_EXPECT(ContainsPath(files, "src/main.cpp"));
    AISTUDIO_EXPECT(ContainsPath(files, "README.md"));
}

AISTUDIO_TEST(FileScanner_Scan_ComputesSizeAndHash) {
    TempProject project;
    const std::string content = "hello world";
    project.WriteFile("a.txt", content);

    const FileScanner scanner;
    const auto result = scanner.Scan(project.RootString());
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().size() == 1);

    const auto& file = result.Value().front();
    AISTUDIO_EXPECT(file.size == content.size());
    AISTUDIO_EXPECT(file.content_hash == HashContent(content));
}

AISTUDIO_TEST(FileScanner_Scan_SkipsDefaultIgnoredDirectories) {
    TempProject project;
    project.WriteFile("src/main.cpp", "code");
    project.WriteFile("node_modules/pkg/index.js", "module.exports = {}");
    project.WriteFile("build/output.obj", "binary");
    project.WriteFile(".git/HEAD", "ref: refs/heads/main");

    const FileScanner scanner;
    const auto result = scanner.Scan(project.RootString());
    AISTUDIO_EXPECT(result.IsOk());

    const auto& files = result.Value();
    AISTUDIO_EXPECT(files.size() == 1);
    AISTUDIO_EXPECT(ContainsPath(files, "src/main.cpp"));
}

AISTUDIO_TEST(FileScanner_Scan_SkipsIgnoredExtensions) {
    TempProject project;
    project.WriteFile("app.exe", "binary");
    project.WriteFile("state.sqlite3", "binary");
    project.WriteFile("app.cpp", "code");

    const FileScanner scanner;
    const auto result = scanner.Scan(project.RootString());
    AISTUDIO_EXPECT(result.IsOk());

    const auto& files = result.Value();
    AISTUDIO_EXPECT(files.size() == 1);
    AISTUDIO_EXPECT(ContainsPath(files, "app.cpp"));
}

AISTUDIO_TEST(FileScanner_Scan_MissingRoot_ReturnsError) {
    const FileScanner scanner;
    const auto result = scanner.Scan((fs::temp_directory_path() / "aistudio_does_not_exist_xyz").string());
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(FileScanner_IsIgnored_CustomPattern) {
    FileScanner::Options options;
    options.ignore_patterns = {"*.generated.ts"};
    const FileScanner scanner(options);

    AISTUDIO_EXPECT(scanner.IsIgnored("src/schema.generated.ts"));
    AISTUDIO_EXPECT(!scanner.IsIgnored("src/schema.ts"));
}

AISTUDIO_TEST(HashContent_SameInput_ProducesSameHash) {
    AISTUDIO_EXPECT(HashContent("abc") == HashContent("abc"));
    AISTUDIO_EXPECT(HashContent("abc") != HashContent("abd"));
}

AISTUDIO_TEST(FileScanner_Scan_WithContentCache_PopulatesItWithoutExtraRead) {
    TempProject project;
    project.WriteFile("a.txt", "hello world");

    FileScanner scanner;
    FileCache cache;
    const auto result = scanner.Scan(project.RootString(), &cache);
    AISTUDIO_EXPECT(result.IsOk());

    const auto& files = result.Value();
    const auto it = std::find_if(files.begin(), files.end(), [](const FileMetadata& m) { return m.path == "a.txt"; });
    AISTUDIO_EXPECT(it != files.end());

    const auto cached = cache.Get(*it);
    AISTUDIO_EXPECT(cached.has_value());
    AISTUDIO_EXPECT(*cached == "hello world");
}

AISTUDIO_TEST(FileScanner_Scan_FindsNonAsciiFileName) {
    // Regression test for the same class of bug already fixed in
    // WorkspaceHash()/Sandbox::Check()/EditorStateStore::Read() (see
    // docs/ROADMAP.md): fs::path's narrow-string construction on Windows
    // goes through CP_ACP, not UTF-8. FileScanner::Scan() itself
    // constructed root_path this way and narrow-round-tripped every
    // relative path via generic_string() before this fix.
    //
    // Written directly via Utf8ToPath rather than TempProject::WriteFile()
    // -- that helper's own root / relative_path append has this same bug
    // for a non-ASCII relative_path (confirmed while writing this test).
    TempProject project;
    const std::string relative = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.cpp"; // 日本語.cpp
    {
        std::ofstream out(Utf8ToPath(project.RootString() + "/" + relative), std::ios::binary);
        out << "int main() {}";
    }

    const FileScanner scanner;
    const auto result = scanner.Scan(project.RootString());
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(ContainsPath(result.Value(), relative));
}

AISTUDIO_TEST(FileScanner_Scan_NoContentCache_BehavesLikeBefore) {
    TempProject project;
    project.WriteFile("a.txt", "hello world");

    FileScanner scanner;
    const auto result = scanner.Scan(project.RootString());
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().size() == 1);
}
