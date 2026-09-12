#include "test_framework.hpp"
#include "Core/Context/ProjectRulesBackend.hpp"
#include "Core/Util/Utf8.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_project_rules_test_" + std::to_string(counter++));
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

ProjectRulesBackend MakeConfiguredBackend(const std::string& root, const std::string& files = "") {
    ProjectRulesBackend backend;
    Config config;
    config.Set("project.root", root);
    if (!files.empty()) {
        config.Set("backend.core.project_rules.files", files);
    }
    backend.Configure(config);
    return backend;
}

} // namespace

AISTUDIO_TEST(ProjectRulesBackend_IdentityFields_AreStable) {
    ProjectRulesBackend backend;
    AISTUDIO_EXPECT(backend.Id() == "core.project_rules");
    AISTUDIO_EXPECT(backend.Health() == BackendHealth::Healthy);
}

AISTUDIO_TEST(ProjectRulesBackend_RulesGet_DefaultFiles_CombinesClaudeMdAndAgentMd) {
    TempProject project;
    project.WriteFile("CLAUDE.md", "no raw pointers\n");
    project.WriteFile("AGENT.md", "small commits\n");

    auto backend = MakeConfiguredBackend(project.RootString());
    Query query;
    query.name = "rules.get";
    const auto result = backend.Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto combined = std::any_cast<std::string>(result.Value());
    AISTUDIO_EXPECT(combined.find("no raw pointers") != std::string::npos);
    AISTUDIO_EXPECT(combined.find("small commits") != std::string::npos);
    AISTUDIO_EXPECT(combined.find("CLAUDE.md") != std::string::npos); // divider names the source file
}

AISTUDIO_TEST(ProjectRulesBackend_RulesGet_NonAsciiFileName_DoesNotThrow) {
    // Regression test for the same class of bug already fixed elsewhere
    // (see docs/ROADMAP.md non-ASCII path crash sweeps): ReadCombinedRules()
    // narrow-constructed root_path and rejoined it with each configured
    // relative path via fs::path's '/' operator on a std::string, both
    // CP_ACP-unsafe on Windows for non-ASCII input.
    //
    // Written directly via Utf8ToPath rather than TempProject::WriteFile()
    // -- that helper's own root / relative_path append has this same bug
    // for a non-ASCII relative_path.
    TempProject project;
    const std::string relative = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.md"; // 日本語.md
    {
        std::ofstream out(Utf8ToPath(project.RootString() + "/" + relative), std::ios::binary);
        out << "no raw pointers\n";
    }

    auto backend = MakeConfiguredBackend(project.RootString(), relative);
    Query query;
    query.name = "rules.get";

    bool threw = false;
    QueryResult result = QueryResult::Fail(Error{});
    try {
        result = backend.Handle(query);
    } catch (const std::exception&) {
        threw = true;
    }
    AISTUDIO_EXPECT(!threw);
    AISTUDIO_EXPECT(result.IsOk());
    const auto combined = std::any_cast<std::string>(result.Value());
    AISTUDIO_EXPECT(combined.find("no raw pointers") != std::string::npos);
}

AISTUDIO_TEST(ProjectRulesBackend_RulesGet_MissingFiles_ReturnsEmptyNotError) {
    TempProject project; // no CLAUDE.md/AGENT.md written -- common case
    auto backend = MakeConfiguredBackend(project.RootString());
    Query query;
    query.name = "rules.get";
    const auto result = backend.Handle(query);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<std::string>(result.Value()).empty());
}

AISTUDIO_TEST(ProjectRulesBackend_RulesGet_CustomFilesList_OnlyReadsConfiguredFiles) {
    TempProject project;
    project.WriteFile("CLAUDE.md", "should not appear\n");
    project.WriteFile("RULES.md", "custom rules content\n");

    auto backend = MakeConfiguredBackend(project.RootString(), "RULES.md");
    Query query;
    query.name = "rules.get";
    const auto result = backend.Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto combined = std::any_cast<std::string>(result.Value());
    AISTUDIO_EXPECT(combined.find("custom rules content") != std::string::npos);
    AISTUDIO_EXPECT(combined.find("should not appear") == std::string::npos);
}

AISTUDIO_TEST(ProjectRulesBackend_Handle_UnknownQuery_Fails) {
    ProjectRulesBackend backend;
    Query query;
    query.name = "not.a.real.query";
    const auto result = backend.Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
}

AISTUDIO_TEST(ProjectRulesBackend_Dispatch_AlwaysFails_ReadOnly) {
    ProjectRulesBackend backend;
    Command command;
    command.name = "rules.set";
    const auto result = backend.Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
}

AISTUDIO_TEST(ProjectRulesBackend_ProvideContext_ReturnsSameContentRegardlessOfIntent) {
    TempProject project;
    project.WriteFile("CLAUDE.md", "no raw pointers\n");

    auto backend = MakeConfiguredBackend(project.RootString());
    const auto items_a = backend.ProvideContext("completely unrelated intent A");
    const auto items_b = backend.ProvideContext("something else entirely B");

    AISTUDIO_EXPECT(items_a.size() == 1);
    AISTUDIO_EXPECT(items_b.size() == 1);
    AISTUDIO_EXPECT(items_a[0].content == items_b[0].content); // ignores intent -- rules apply universally
    AISTUDIO_EXPECT(items_a[0].content.find("no raw pointers") != std::string::npos);
}

AISTUDIO_TEST(ProjectRulesBackend_ProvideContext_EmptyIntent_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("CLAUDE.md", "no raw pointers\n");
    auto backend = MakeConfiguredBackend(project.RootString());
    AISTUDIO_EXPECT(backend.ProvideContext("").empty());
}

AISTUDIO_TEST(ProjectRulesBackend_ProvideContext_NoRulesFilesExist_ReturnsEmpty) {
    TempProject project;
    auto backend = MakeConfiguredBackend(project.RootString());
    AISTUDIO_EXPECT(backend.ProvideContext("some intent").empty());
}
