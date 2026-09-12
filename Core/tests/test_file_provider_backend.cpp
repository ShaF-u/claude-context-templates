#include "test_framework.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Context/ContextRetriever.hpp"
#include "Core/Context/FileProviderBackend.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Security/Sandbox.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_file_provider_test_" + std::to_string(counter++));
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

bool HasId(const std::vector<ContextItem>& items, const std::string& id) {
    return std::any_of(items.begin(), items.end(), [&](const ContextItem& item) { return item.id == id; });
}

const ContextItem* FindById(const std::vector<ContextItem>& items, const std::string& id) {
    const auto it = std::find_if(items.begin(), items.end(), [&](const ContextItem& item) { return item.id == id; });
    return it == items.end() ? nullptr : &*it;
}

Config ConfigWithRoot(const std::string& root) {
    Config config;
    config.Set("project.root", root);
    return config;
}

} // namespace

AISTUDIO_TEST(FileProviderBackend_IdentityFields_AreStable) {
    FileProviderBackend backend;
    AISTUDIO_EXPECT(backend.Id() == "core.file_provider");
    AISTUDIO_EXPECT(backend.Name() == "File Context Provider");
    AISTUDIO_EXPECT(!backend.Version().empty());
    const auto capabilities = backend.Capabilities();
    AISTUDIO_EXPECT(std::find(capabilities.begin(), capabilities.end(), "context.provide.file@1.0.0") !=
                     capabilities.end());
    AISTUDIO_EXPECT(backend.Health() == BackendHealth::Healthy);
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_BeforeConfigure_ReturnsEmpty) {
    const FileProviderBackend backend;
    AISTUDIO_EXPECT(backend.ProvideContext("anything").empty());
}

AISTUDIO_TEST(FileProviderBackend_Configure_FallsBackToProjectRoot) {
    TempProject project;
    project.WriteFile("Notes.txt", "the quick brown fox");

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));

    const auto items = backend.ProvideContext("quick brown fox");
    AISTUDIO_EXPECT(HasId(items, "Notes.txt"));
}

AISTUDIO_TEST(FileProviderBackend_Configure_ExplicitRootOverridesProjectRoot) {
    TempProject wrong_project;
    wrong_project.WriteFile("Notes.txt", "should not be found");

    TempProject right_project;
    right_project.WriteFile("Notes.txt", "the quick brown fox");

    Config config = ConfigWithRoot(wrong_project.RootString());
    config.Set("backend.core.file_provider.root", right_project.RootString());

    FileProviderBackend backend;
    backend.Configure(config);

    const auto items = backend.ProvideContext("quick brown fox");
    const auto* item = FindById(items, "Notes.txt");
    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->content == "the quick brown fox");
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_NoOccurrences_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("Notes.txt", "the quick brown fox");

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));

    AISTUDIO_EXPECT(backend.ProvideContext("nothing this file contains").empty());
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_MatchingFile_ReturnsRealContentAndSourceFile) {
    TempProject project;
    project.WriteFile("PlayerAttack.cpp", "void PlayerAttack() { DoAttack(); DoAttack(); }\n");

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));

    const auto items = backend.ProvideContext("DoAttack");
    const auto* item = FindById(items, "PlayerAttack.cpp");
    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->source == ContextSourceKind::File);
    AISTUDIO_EXPECT(item->content == "void PlayerAttack() { DoAttack(); DoAttack(); }\n");
    AISTUDIO_EXPECT(item->priority >= 20 && item->priority <= 65);
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_NonAsciiFileName_DoesNotThrow) {
    // Regression test for the same class of bug already fixed elsewhere
    // (see docs/ROADMAP.md non-ASCII path crash sweeps): ProvideContext()
    // narrow-constructed root_path and rejoined it with each scanned
    // relative path via fs::path's '/' operator on a std::string, both
    // CP_ACP-unsafe on Windows for non-ASCII input.
    //
    // Written directly via Utf8ToPath rather than TempProject::WriteFile()
    // -- that helper's own root / relative_path append has this same bug
    // for a non-ASCII relative_path.
    TempProject project;
    const std::string relative = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt"; // 日本語.txt
    {
        std::ofstream out(Utf8ToPath(project.RootString() + "/" + relative), std::ios::binary);
        out << "DoAttack DoAttack";
    }

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));

    bool threw = false;
    std::vector<ContextItem> items;
    try {
        items = backend.ProvideContext("DoAttack");
    } catch (const std::exception&) {
        threw = true;
    }
    AISTUDIO_EXPECT(!threw);
    AISTUDIO_EXPECT(HasId(items, relative));
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_ReusesScanUntilFileChanged) {
    // docs/ROADMAP.md CE-3 leftover: ProvideContext() used to re-scan the
    // whole project on every call. Now it caches the scan and only
    // rescans after "FileChanged" -- verified here via the real
    // Start()/EventBus path, not a white-box cache reset.
    TempProject project;
    project.WriteFile("A.txt", "widget");

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));
    backend.Start();

    AISTUDIO_EXPECT(HasId(backend.ProvideContext("widget"), "A.txt"));

    project.WriteFile("B.txt", "widget");
    AISTUDIO_EXPECT(!HasId(backend.ProvideContext("widget"), "B.txt")); // stale cached scan

    EventBus::Instance().Publish("FileChanged");
    AISTUDIO_EXPECT(HasId(backend.ProvideContext("widget"), "B.txt")); // rescanned

    backend.Stop();
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_CapsAtMaxResults) {
    TempProject project;
    Config config = ConfigWithRoot(project.RootString());
    config.Set("backend.core.file_provider.max_results", "2");

    project.WriteFile("A.txt", "widget widget widget widget");
    project.WriteFile("B.txt", "widget widget widget");
    project.WriteFile("C.txt", "widget widget");
    project.WriteFile("D.txt", "widget");

    FileProviderBackend backend;
    backend.Configure(config);

    const auto items = backend.ProvideContext("widget");
    AISTUDIO_EXPECT(items.size() == 2);
    // Highest-occurrence files win the cap.
    AISTUDIO_EXPECT(HasId(items, "A.txt"));
    AISTUDIO_EXPECT(HasId(items, "B.txt"));
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_OversizedFile_IsSkipped) {
    TempProject project;
    // kMaxFileSizeBytes is 256 KiB — write something well past that.
    project.WriteFile("Huge.txt", std::string(300 * 1024, 'x') + " findme");

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));

    AISTUDIO_EXPECT(!HasId(backend.ProvideContext("findme"), "Huge.txt"));
}

// docs/ROADMAP.md CE-5: a real .jar in this repo (Tools/rider-plugin/
// gradle/wrapper/gradle-wrapper.jar) has "gradle"/"wrapper" readable in
// its ZIP central directory alongside binary compressed data, which used
// to crash the whole response at JSON serialization (invalid UTF-8)
// instead of just not matching. Reproduces that shape without depending
// on the actual repo file.
AISTUDIO_TEST(FileProviderBackend_ProvideContext_BinaryFileContainingIntent_IsSkippedNotCrashed) {
    TempProject project;
    std::string binary_content = "findme";
    binary_content += '\xFA';
    binary_content += '\x17';
    binary_content += "more binary bytes";
    project.WriteFile("asset.bin", binary_content);

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));

    AISTUDIO_EXPECT(!HasId(backend.ProvideContext("findme"), "asset.bin"));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_BackendRegistryOnly_SurfacesFileProviderContent) {
    TempProject project;
    project.WriteFile("PlayerAttack.cpp", "void PlayerAttack() { DoAttack(); }\n");

    auto backend = std::make_shared<FileProviderBackend>();
    backend->Configure(ConfigWithRoot(project.RootString()));

    BackendRegistry registry;
    registry.Register(backend);

    // project_root deliberately left unset — proves this path doesn't
    // depend on ContextRetriever's own built-in File retrieval step.
    ContextRetriever::Options options;
    options.backend_registry = &registry;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("DoAttack");
    const auto* item = FindById(items, "PlayerAttack.cpp");
    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->source == ContextSourceKind::File);
    AISTUDIO_EXPECT(!item->content.empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_FirewallBlocksFileProviderItem) {
    TempProject project;
    project.WriteFile("secrets.txt", "the password is hunter2");

    auto backend = std::make_shared<FileProviderBackend>();
    backend->Configure(ConfigWithRoot(project.RootString()));

    BackendRegistry registry;
    registry.Register(backend);

    const Sandbox firewall(project.RootString()); // "*secret*" is a default deny pattern

    ContextRetriever::Options options;
    options.backend_registry = &registry;
    options.firewall = &firewall;
    const ContextRetriever retriever(options);

    AISTUDIO_EXPECT(!HasId(retriever.Retrieve("password"), "secrets.txt"));
}

// --- Excerpts instead of whole files (MASTER_SPEC #99) ---

namespace {

std::string NumberedLines(int total, int match_line, const std::string& needle) {
    std::string content;
    for (int line = 1; line <= total; ++line) {
        content += "line" + std::to_string(line);
        if (line == match_line) {
            content += " " + needle;
        }
        content += "\n";
    }
    return content;
}

} // namespace

AISTUDIO_TEST(FileProviderBackend_ProvideContext_LongFile_ReturnsExcerptNotWholeFile) {
    TempProject project;
    project.WriteFile("big.cpp", NumberedLines(300, 150, "TargetIntent"));

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));
    const auto items = backend.ProvideContext("TargetIntent");

    const auto* item = FindById(items, "big.cpp");
    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->compression == CompressionLevel::Summary);
    AISTUDIO_EXPECT(item->content.find("@@ big.cpp:130-170 @@") != std::string::npos);
    AISTUDIO_EXPECT(item->content.find("line150 TargetIntent") != std::string::npos);
    // Lines far from the match are absent -- that is the saving.
    AISTUDIO_EXPECT(item->content.find("line1\n") == std::string::npos);
    AISTUDIO_EXPECT(item->content.find("line300") == std::string::npos);
    AISTUDIO_EXPECT(item->content.size() < NumberedLines(300, 150, "TargetIntent").size());
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_EstimatedTokensReflectTheExcerpt) {
    TempProject project;
    const auto content = NumberedLines(300, 150, "TargetIntent");
    project.WriteFile("big.cpp", content);

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));
    const auto items = backend.ProvideContext("TargetIntent");
    const auto* item = FindById(items, "big.cpp");

    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->estimated_tokens == EstimateTokens(item->content));
    AISTUDIO_EXPECT(item->estimated_tokens < EstimateTokens(content));
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_ShortFile_ComesBackWholeAndRaw) {
    TempProject project;
    // A +/-20 window already covers 5 lines, so no header is added.
    project.WriteFile("small.cpp", NumberedLines(5, 3, "TargetIntent"));

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));
    const auto items = backend.ProvideContext("TargetIntent");
    const auto* item = FindById(items, "small.cpp");

    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->compression == CompressionLevel::Raw);
    AISTUDIO_EXPECT(item->content.find("@@") == std::string::npos);
    AISTUDIO_EXPECT(item->content == NumberedLines(5, 3, "TargetIntent"));
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_SeparateMatches_ProduceSeparateBlocks) {
    TempProject project;
    std::string content;
    for (int line = 1; line <= 300; ++line) {
        content += "line" + std::to_string(line);
        if (line == 10 || line == 200) {
            content += " TargetIntent";
        }
        content += "\n";
    }
    project.WriteFile("two.cpp", content);

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));
    const auto items = backend.ProvideContext("TargetIntent");
    const auto* item = FindById(items, "two.cpp");

    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->content.find("@@ two.cpp:1-30 @@") != std::string::npos);
    AISTUDIO_EXPECT(item->content.find("@@ two.cpp:180-220 @@") != std::string::npos);
    AISTUDIO_EXPECT(item->content.find("line100") == std::string::npos);
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_ContextLinesIsConfigurable) {
    TempProject project;
    project.WriteFile("big.cpp", NumberedLines(300, 150, "TargetIntent"));

    FileProviderBackend backend;
    Config config = ConfigWithRoot(project.RootString());
    config.Set("backend.core.file_provider.context_lines", "2");
    backend.Configure(config);

    const auto items = backend.ProvideContext("TargetIntent");
    const auto* item = FindById(items, "big.cpp");
    AISTUDIO_EXPECT(item != nullptr);
    AISTUDIO_EXPECT(item->content.find("@@ big.cpp:148-152 @@") != std::string::npos);
    AISTUDIO_EXPECT(item->content.find("line147") == std::string::npos);
}

AISTUDIO_TEST(FileProviderBackend_ProvideContext_ScoringStillCountsEveryOccurrence) {
    TempProject project;
    // Two hits on separate lines must sum, not count matching lines.
    project.WriteFile("a.cpp", "TargetIntent\nTargetIntent TargetIntent\n");
    project.WriteFile("b.cpp", "TargetIntent\n");

    FileProviderBackend backend;
    backend.Configure(ConfigWithRoot(project.RootString()));
    const auto items = backend.ProvideContext("TargetIntent");

    const auto* a = FindById(items, "a.cpp");
    const auto* b = FindById(items, "b.cpp");
    AISTUDIO_EXPECT(a != nullptr && b != nullptr);
    AISTUDIO_EXPECT(a->priority > b->priority);
}
