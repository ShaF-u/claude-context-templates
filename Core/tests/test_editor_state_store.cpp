#include "test_framework.hpp"
#include "Core/IDE/EditorStateStore.hpp"
#include "Core/Security/Sandbox.hpp"
#include "Core/Util/WorkspaceHash.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_editor_state_test_" + std::to_string(counter++));
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
    [[nodiscard]] std::string Path(const std::string& relative_path) const { return (root / relative_path).string(); }
};

// A state-file path unique per test run, outside any TempProject (this
// class reads a single fixed file path in real use -- tests need their
// own path per case so parallel/sequential test runs never race on the
// same file another test, or a real IDE extension/`--mcp` process on the
// same machine, might also be touching).
struct TempStateFile {
    fs::path path;

    TempStateFile() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / fs::path("aistudio_editor_state_test_file_" + std::to_string(counter++) + ".json");
        fs::remove(path);
    }

    ~TempStateFile() { fs::remove(path); }

    void Write(const std::string& json_text) const {
        std::ofstream out(path, std::ios::binary);
        out << json_text;
    }

    [[nodiscard]] std::string PathString() const { return path.string(); }
};

} // namespace

AISTUDIO_TEST(EditorStateStore_Read_NoFile_ReturnsNullopt) {
    TempStateFile state_file;
    // Never written -- file doesn't exist.
    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});
    AISTUDIO_EXPECT(!store.Read().has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_MalformedJson_ReturnsNullopt) {
    TempStateFile state_file;
    state_file.Write("{ not valid json");
    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});
    AISTUDIO_EXPECT(!store.Read().has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_UnrecognizedVersion_ReturnsNullopt) {
    TempStateFile state_file;
    state_file.Write(R"({"version": 999, "active_document_path": "a.cpp", "selection": null})");
    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});
    AISTUDIO_EXPECT(!store.Read().has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_MissingRequiredField_ReturnsNullopt) {
    TempStateFile state_file;
    state_file.Write(R"({"version": 1, "selection": null})"); // no active_document_path key at all
    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});
    AISTUDIO_EXPECT(!store.Read().has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_NoActiveDocument_ReturnsStateWithNulloptPath) {
    TempStateFile state_file;
    state_file.Write(R"({"version": 1, "source": "vscode", "active_document_path": null, "selection": null,
                          "updated_at": "2026-09-05T00:00:00.000Z"})");
    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(!state->active_document_path.has_value());
    AISTUDIO_EXPECT(!state->selection.has_value());
    AISTUDIO_EXPECT(state->source == "vscode");
    AISTUDIO_EXPECT(state->updated_at == "2026-09-05T00:00:00.000Z");
}

AISTUDIO_TEST(EditorStateStore_Read_ActiveDocumentNoSelection_ReturnsPathWithNulloptSelection) {
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}");
    TempStateFile state_file;
    // Built via nlohmann::json (not raw string concatenation) so a
    // Windows absolute path's backslashes get JSON-escaped correctly --
    // TempProject::Path() returns a native, backslash-separated path.
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"active_document_path", project.Path("src/main.cpp")},
        {"selection", nullptr},
        {"updated_at", ""},
    }.dump());

    const Sandbox sandbox(project.RootString());
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
        .firewall = &sandbox,
    });

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->active_document_path.has_value());
    AISTUDIO_EXPECT(*state->active_document_path == "src/main.cpp");
    AISTUDIO_EXPECT(!state->selection.has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_ActiveDocumentWithSelection_ReturnsSelection) {
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}");
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"active_document_path", project.Path("src/main.cpp")},
        {"selection", Json{{"start_line", 3}, {"start_character", 1}, {"end_line", 5}, {"end_character", 9}}},
        {"updated_at", ""},
    }.dump());

    const Sandbox sandbox(project.RootString());
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
        .firewall = &sandbox,
    });

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->selection.has_value());
    AISTUDIO_EXPECT(state->selection->start_line == 3);
    AISTUDIO_EXPECT(state->selection->start_character == 1);
    AISTUDIO_EXPECT(state->selection->end_line == 5);
    AISTUDIO_EXPECT(state->selection->end_character == 9);
}

AISTUDIO_TEST(EditorStateStore_Read_PathOutsideSandboxRoot_IsDroppedNotError) {
    TempProject project;
    TempProject outside; // a second, unrelated root -- simulates a path outside project.root
    outside.WriteFile("secret.txt", "shh");
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"active_document_path", outside.Path("secret.txt")},
        {"selection", nullptr},
        {"updated_at", ""},
    }.dump());

    const Sandbox sandbox(project.RootString()); // firewall rooted at `project`, not `outside`
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
        .firewall = &sandbox,
    });

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value()); // file itself is well-formed
    AISTUDIO_EXPECT(!state->active_document_path.has_value()); // but the path was silently dropped
    AISTUDIO_EXPECT(!state->selection.has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_NoFirewallConfigured_AppliesNoFiltering) {
    TempProject outside;
    outside.WriteFile("secret.txt", "shh");
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"active_document_path", outside.Path("secret.txt")},
        {"selection", nullptr},
        {"updated_at", ""},
    }.dump());

    // No project_root, no firewall -- path passed through exactly as reported.
    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->active_document_path.has_value());
    AISTUDIO_EXPECT(*state->active_document_path == outside.Path("secret.txt"));
}

AISTUDIO_TEST(EditorStateStore_Read_NonAsciiActiveDocumentPath_DoesNotThrow) {
    // Regression test for the same class of bug fixed in WorkspaceHash()
    // and Sandbox::Check() (docs/ROADMAP.md "重大バグ発見・修正..."):
    // fs::path's narrow-string construction on Windows goes through the
    // system ANSI code page (CP_ACP), not UTF-8, and can throw for
    // non-ASCII input. ToProjectRelativePath() (this file's anonymous
    // namespace) constructs fs::path directly from `path`/`project_root`
    // -- both attacker/IDE-controlled -- so a Japanese active_document_path
    // is exactly the input that would trigger it.
    //
    // Built with plain string concatenation, deliberately NOT via
    // TempProject::WriteFile()/Path() -- those go through fs::path's own
    // '/' operator on a std::string, which has this exact same bug
    // (confirmed while writing this test), so using them here would test
    // the fixture's bug instead of EditorStateStore's. The file doesn't
    // need to actually exist on disk -- weakly_canonical() (what
    // ToProjectRelativePath() uses) works on nonexistent paths too.
    TempProject project;
    const std::string non_ascii_path = project.RootString() + "/\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.txt"; // 日本語.txt
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"active_document_path", non_ascii_path},
        {"selection", nullptr},
        {"updated_at", ""},
    }.dump());

    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
    });

    bool threw = false;
    std::optional<EditorState> state;
    try {
        state = store.Read();
    } catch (const std::exception&) {
        threw = true;
    }
    AISTUDIO_EXPECT(!threw);
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->active_document_path.has_value());
}

// docs/ROADMAP.md Phase 13 "固定ファイル名のIDE状態": Tools/vscode-extension/
// src/editorState.ts (and the Visual Studio/Rider counterparts) already
// write an unused `workspace_root` field into every snapshot. These tests
// exercise the WorkspaceRootMismatch check that now consumes it -- a gap
// the Sandbox firewall alone doesn't close, since a nested/sibling
// project's file can pass the firewall's own path-containment check.
AISTUDIO_TEST(EditorStateStore_Read_WorkspaceRootMismatch_ReturnsNullopt) {
    TempProject project;
    TempProject other_project; // a DIFFERENT workspace, e.g. a sibling monorepo subproject
    project.WriteFile("src/main.cpp", "int main() {}");
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"workspace_root", other_project.RootString()}, // reports a different workspace than `project`
        {"active_document_path", project.Path("src/main.cpp")}, // path itself is inside `project`'s root
        {"selection", nullptr},
        {"updated_at", "2026-09-09T00:00:00.000Z"},
    }.dump());

    const Sandbox sandbox(project.RootString());
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
        .firewall = &sandbox,
    });

    // Firewall alone would have accepted this (the path IS inside
    // `project`'s root) -- the workspace_root mismatch is what rejects it.
    AISTUDIO_EXPECT(!store.Read().has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_WorkspaceRootMatches_StateIsUsable) {
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}");
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"workspace_root", project.RootString()}, // matches this store's own project_root
        {"active_document_path", project.Path("src/main.cpp")},
        {"selection", nullptr},
        {"updated_at", ""},
    }.dump());

    const Sandbox sandbox(project.RootString());
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
        .firewall = &sandbox,
    });

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->active_document_path.has_value());
    AISTUDIO_EXPECT(*state->active_document_path == "src/main.cpp");
}

AISTUDIO_TEST(EditorStateStore_Read_NoWorkspaceRootField_AppliesNoFiltering) {
    // An older/hypothetical writer that hasn't been updated to write
    // workspace_root yet -- must not be rejected outright (fails open).
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}");
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"active_document_path", project.Path("src/main.cpp")},
        {"selection", nullptr},
        {"updated_at", ""},
    }.dump());

    const Sandbox sandbox(project.RootString());
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
        .firewall = &sandbox,
    });

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->active_document_path.has_value());
}

AISTUDIO_TEST(EditorStateStore_Read_WorkspaceRootMismatch_NoProjectRootConfigured_AppliesNoFiltering) {
    // project_root itself wasn't configured on this store -- nothing to
    // compare workspace_root against, so the check is a no-op (consistent
    // with the firewall's own "nullptr applies no filtering").
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}");
    TempProject other_project;
    TempStateFile state_file;
    state_file.Write(Json{
        {"version", 1},
        {"source", "vscode"},
        {"workspace_root", other_project.RootString()},
        {"active_document_path", project.Path("src/main.cpp")},
        {"selection", nullptr},
        {"updated_at", ""},
    }.dump());

    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});

    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->active_document_path.has_value());
}

AISTUDIO_TEST(EditorStateStore_DefaultStateFilePath_IsUnderTempDirectory) {
    const auto path = EditorStateStore::DefaultStateFilePath();
    const auto temp_dir = std::filesystem::temp_directory_path().string();
    AISTUDIO_EXPECT(path.rfind(temp_dir, 0) == 0);
    AISTUDIO_EXPECT(path.find("aistudio_editor_state.json") != std::string::npos);
}

AISTUDIO_TEST(EditorStateStore_DefaultStateFilePath_WithWorkspaceRoot_UsesHashedFileName) {
    const auto workspace_root = std::filesystem::temp_directory_path().string();
    const auto path = EditorStateStore::DefaultStateFilePath(workspace_root);
    // Not the legacy fixed name -- and matches Core::WorkspaceHash's own
    // output for the same input, since DefaultStateFilePath() must use
    // that exact function (docs/ROADMAP.md "固定ファイル名のIDE状態").
    AISTUDIO_EXPECT(path.find("aistudio_editor_state_" + WorkspaceHash(workspace_root) + ".json") != std::string::npos);
}

AISTUDIO_TEST(EditorStateStore_DefaultStateFilePath_DifferentWorkspaceRoots_ProduceDifferentPaths) {
    const auto temp_dir_path = EditorStateStore::DefaultStateFilePath(std::filesystem::temp_directory_path().string());
    const auto current_dir_path = EditorStateStore::DefaultStateFilePath(std::filesystem::current_path().string());
    AISTUDIO_EXPECT(temp_dir_path != current_dir_path);
}

AISTUDIO_TEST(EditorStateStore_Constructor_WithProjectRoot_ActuallyReadsFromHashedPath) {
    TempProject project;
    project.WriteFile("a.cpp", "int main() {}");
    const std::string hashed_path = EditorStateStore::DefaultStateFilePath(project.RootString());
    fs::remove(hashed_path);

    // Built via nlohmann::json, not raw string concatenation -- see the
    // comment on EditorStateStore_Read_ActiveDocumentNoSelection_
    // ReturnsPathWithNulloptSelection above for why (a Windows absolute
    // path's backslashes need JSON escaping).
    {
        std::ofstream out(hashed_path, std::ios::binary);
        out << Json{
                    {"version", 1},
                    {"source", "vscode"},
                    {"active_document_path", project.Path("a.cpp")},
                    {"selection", nullptr},
                    {"workspace_root", project.RootString()},
                    {"updated_at", ""},
                }
                   .dump();
    }

    // state_file_path left empty on purpose -- this is the point of the
    // test: the constructor must derive the hashed path from
    // project_root on its own, not just accept whatever's passed.
    const EditorStateStore store(EditorStateStore::Options{.project_root = project.RootString()});
    const auto state = store.Read();
    AISTUDIO_EXPECT(state.has_value());
    AISTUDIO_EXPECT(state->active_document_path.has_value());

    fs::remove(hashed_path);
}
