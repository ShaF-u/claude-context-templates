#include "test_framework.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Context/ContextRetriever.hpp"
#include "Core/Git/GitBackend.hpp"
#include "Core/Security/Sandbox.hpp"
#include "Core/Util/ProcessRunner.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

struct TempGitRepo {
    fs::path root;

    TempGitRepo() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_git_backend_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
        RunGit({"init"});
        RunGit({"config", "user.email", "test@example.com"});
        RunGit({"config", "user.name", "Test"});
    }

    ~TempGitRepo() { fs::remove_all(root); }

    void WriteFile(const std::string& relative_path, const std::string& content) const {
        const fs::path full_path = root / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
    }

    void RunGit(const std::vector<std::string>& args) const { (void)RunProcess("git", args, root.string()); }

    void Commit(const std::string& message) const {
        RunGit({"add", "-A"});
        RunGit({"commit", "-m", message});
    }

    [[nodiscard]] std::string RootString() const { return root.string(); }
};

std::shared_ptr<GitBackend> MakeStartedBackend(const std::string& root) {
    auto backend = std::make_shared<GitBackend>();
    Config config;
    config.Set("project.root", root);
    backend->Configure(config);
    backend->Start();
    return backend;
}

// A fresh, not-yet-existing sibling directory for `git worktree add` to
// create -- worktree tests below remove it themselves once done (unlike
// TempGitRepo, nothing else owns its lifetime).
struct TempWorktreePath {
    fs::path path;

    TempWorktreePath() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / fs::path("aistudio_git_backend_worktree_" + std::to_string(counter++));
        fs::remove_all(path);
    }

    ~TempWorktreePath() { fs::remove_all(path); }

    [[nodiscard]] std::string String() const { return path.string(); }
};

} // namespace

AISTUDIO_TEST(GitBackend_IdentityFields_AreStable) {
    GitBackend backend;
    AISTUDIO_EXPECT(backend.Id() == "core.git");
    AISTUDIO_EXPECT(backend.Name() == "Git Backend");
    const auto capabilities = backend.Capabilities();
    AISTUDIO_EXPECT(std::find(capabilities.begin(), capabilities.end(), "git.status@1.0.0") != capabilities.end());
    AISTUDIO_EXPECT(std::find(capabilities.begin(), capabilities.end(), "git.diff@1.0.0") != capabilities.end());
    AISTUDIO_EXPECT(std::find(capabilities.begin(), capabilities.end(), "git.history@1.0.0") != capabilities.end());
    AISTUDIO_EXPECT(std::find(capabilities.begin(), capabilities.end(), "context.provide.git@1.0.0") !=
                     capabilities.end());
}

AISTUDIO_TEST(GitBackend_Start_WithGitInstalled_HealthIsHealthy) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    AISTUDIO_EXPECT(backend->Health() == BackendHealth::Healthy);
}

AISTUDIO_TEST(GitBackend_Handle_BeforeStart_Fails) {
    GitBackend backend; // never Configure()'d/Start()'d -- available_ stays false
    Query query;
    query.name = "git.status";
    const auto result = backend.Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
}

AISTUDIO_TEST(GitBackend_Dispatch_UnknownCommand_Fails) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.push"; // out of scope -- see GitBackend.hpp's own comment
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(GitBackend_Dispatch_BeforeStart_Fails) {
    GitBackend backend; // never Configure()'d/Start()'d -- available_ stays false
    Command command;
    command.name = "git.commit";
    command.payload = std::any(std::string("message"));
    const auto result = backend.Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
}

AISTUDIO_TEST(GitBackend_GitCommit_EmptyMessage_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.commit";
    command.payload = std::any(std::string(""));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitCommit_NoPayload_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.commit"; // payload left empty (no std::string set)
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitCommit_StagesAndCommitsAllChanges) {
    TempGitRepo repo;
    repo.WriteFile("committed.txt", "hello\n");
    repo.Commit("initial commit");
    repo.WriteFile("new.txt", "brand new\n"); // untracked
    repo.WriteFile("committed.txt", "hello\nmodified\n"); // modified, unstaged

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.commit";
    command.payload = std::any(std::string("commit everything"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());

    Query history_query;
    history_query.name = "git.history";
    const auto history_result = backend->Handle(history_query);
    AISTUDIO_EXPECT(history_result.IsOk());
    const auto commits = std::any_cast<std::vector<GitCommitEntry>>(history_result.Value());
    AISTUDIO_EXPECT(commits.size() == 2);
    AISTUDIO_EXPECT(commits[0].subject == "commit everything");

    Query status_query;
    status_query.name = "git.status";
    const auto status_result = backend->Handle(status_query);
    AISTUDIO_EXPECT(status_result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<GitStatusResult>(status_result.Value()).entries.empty());
}

AISTUDIO_TEST(GitBackend_GitBranch_EmptyName_IsRejected) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.branch";
    command.payload = std::any(std::string(""));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

// Regression test for a real argument-injection bug found during a
// post-merge implementation review: "git branch -a" (branch_name="-a")
// doesn't create anything -- it silently lists branches instead, and
// still returns Ok() with that listing as if a branch had been created.
AISTUDIO_TEST(GitBackend_GitBranch_NameStartingWithDash_IsRejected) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.branch";
    command.payload = std::any(std::string("-a"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);

    const auto list_result = RunProcess("git", {"branch", "--list"}, repo.RootString());
    AISTUDIO_EXPECT(list_result.IsOk());
    AISTUDIO_EXPECT(list_result.Value().output.find("-a") == std::string::npos); // nothing named "-a" got created
}

AISTUDIO_TEST(GitBackend_GitBranch_CreatesBranch_WithoutSwitchingToIt) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.branch";
    command.payload = std::any(std::string("feature/new-thing"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());

    const auto list_result = RunProcess("git", {"branch", "--list"}, repo.RootString());
    AISTUDIO_EXPECT(list_result.IsOk());
    AISTUDIO_EXPECT(list_result.Value().output.find("feature/new-thing") != std::string::npos);

    Query status_query;
    status_query.name = "git.status";
    const auto status_result = backend->Handle(status_query);
    AISTUDIO_EXPECT(status_result.IsOk());
    // still on the original branch -- git.branch creates only, never switches
    AISTUDIO_EXPECT(std::any_cast<GitStatusResult>(status_result.Value()).branch != "feature/new-thing");
}

AISTUDIO_TEST(GitBackend_GitStash_CleansWorkingTree) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.WriteFile("f.txt", "1\nmodified\n");

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.stash";
    command.payload = std::any(std::string("wip"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());

    Query status_query;
    status_query.name = "git.status";
    const auto status_result = backend->Handle(status_query);
    AISTUDIO_EXPECT(status_result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<GitStatusResult>(status_result.Value()).entries.empty());

    const auto stash_list = RunProcess("git", {"stash", "list"}, repo.RootString());
    AISTUDIO_EXPECT(stash_list.IsOk());
    AISTUDIO_EXPECT(stash_list.Value().output.find("wip") != std::string::npos);
}

AISTUDIO_TEST(GitBackend_GitStash_NoMessage_StillStashes) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.WriteFile("f.txt", "1\nmodified\n");

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.stash"; // no payload -- optional message
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());

    Query status_query;
    status_query.name = "git.status";
    const auto status_result = backend->Handle(status_query);
    AISTUDIO_EXPECT(status_result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<GitStatusResult>(status_result.Value()).entries.empty());
}

AISTUDIO_TEST(GitBackend_GitCheckout_EmptyBranchName_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.checkout";
    command.payload = std::any(std::string(""));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitCheckout_SwitchesToExistingBranch) {
    TempGitRepo repo;
    repo.RunGit({"checkout", "-b", "base"});
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.RunGit({"branch", "feature"});

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.checkout";
    command.payload = std::any(std::string("feature"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());

    Query status_query;
    status_query.name = "git.status";
    const auto status_result = backend->Handle(status_query);
    AISTUDIO_EXPECT(status_result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<GitStatusResult>(status_result.Value()).branch == "feature");
}

AISTUDIO_TEST(GitBackend_GitCheckout_NonexistentBranch_Fails) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.checkout";
    command.payload = std::any(std::string("no-such-branch"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
}

AISTUDIO_TEST(GitBackend_GitCheckout_NeverForces_RefusesWhenChangesWouldBeOverwritten) {
    TempGitRepo repo;
    repo.RunGit({"checkout", "-b", "base"});
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.RunGit({"checkout", "-b", "feature"});
    repo.WriteFile("f.txt", "1\nfeature change\n");
    repo.Commit("feature commit");
    repo.RunGit({"checkout", "base"});
    // Conflicting unstaged change on "base" that "feature" would overwrite.
    repo.WriteFile("f.txt", "1\nconflicting uncommitted change\n");

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.checkout";
    command.payload = std::any(std::string("feature"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk()); // git itself refuses -- no --force was passed

    Query status_query;
    status_query.name = "git.status";
    const auto status_result = backend->Handle(status_query);
    AISTUDIO_EXPECT(status_result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<GitStatusResult>(status_result.Value()).branch == "base"); // still on base
}

// Regression test for a real argument-injection bug found during a
// post-merge implementation review: branch_name="-f" makes GitBackend
// run "git checkout -f" (no other argument), which git interprets as
// "force-discard uncommitted changes on the current branch" -- silently
// destroying work, the exact outcome "never forces" was written to
// prevent. Confirmed against a real git binary before this fix existed:
// the uncommitted change was gone and the command reported success.
AISTUDIO_TEST(GitBackend_GitCheckout_BranchNameStartingWithDash_IsRejected_NotTreatedAsForceFlag) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.WriteFile("f.txt", "1\nimportant uncommitted work\n");

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.checkout";
    command.payload = std::any(std::string("-f"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);

    // The uncommitted work must still be there -- "-f" must NOT have
    // reached git as a bare "--force" checkout.
    Query status_query;
    status_query.name = "git.status";
    const auto status_result = backend->Handle(status_query);
    AISTUDIO_EXPECT(status_result.IsOk());
    AISTUDIO_EXPECT(!std::any_cast<GitStatusResult>(status_result.Value()).entries.empty());
}

AISTUDIO_TEST(GitBackend_GitTag_EmptyTagName_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.tag";
    command.payload = std::any(std::string(""));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitTag_CreatesTagAtHead) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.tag";
    command.payload = std::any(std::string("v1.0.0"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());

    const auto list_result = RunProcess("git", {"tag", "--list"}, repo.RootString());
    AISTUDIO_EXPECT(list_result.IsOk());
    AISTUDIO_EXPECT(list_result.Value().output.find("v1.0.0") != std::string::npos);
}

AISTUDIO_TEST(GitBackend_GitTag_DuplicateName_Fails) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.RunGit({"tag", "v1.0.0"}); // already exists

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.tag";
    command.payload = std::any(std::string("v1.0.0"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk()); // git itself refuses -- no -f was passed
}

// Regression test (same class as GitBackend_GitBranch_NameStartingWithDash_IsRejected).
AISTUDIO_TEST(GitBackend_GitTag_NameStartingWithDash_IsRejected) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.tag";
    command.payload = std::any(std::string("--list"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitWorktreeAdd_CreatesWorktreeWithNewBranch) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    TempWorktreePath worktree;

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.add";
    command.payload = std::any(std::make_pair(std::string("feature/session-1"), worktree.String()));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());

    AISTUDIO_EXPECT(fs::exists(worktree.path / "f.txt")); // new worktree checked out from HEAD

    const auto branch_list = RunProcess("git", {"branch", "--list"}, repo.RootString());
    AISTUDIO_EXPECT(branch_list.IsOk());
    AISTUDIO_EXPECT(branch_list.Value().output.find("feature/session-1") != std::string::npos);
}

AISTUDIO_TEST(GitBackend_GitWorktreeAdd_WrongPayloadType_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.add";
    command.payload = std::any(std::string("just_a_string"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitWorktreeAdd_EmptyBranchName_IsRejected) {
    TempGitRepo repo;
    TempWorktreePath worktree;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.add";
    command.payload = std::any(std::make_pair(std::string(""), worktree.String()));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitWorktreeAdd_EmptyPath_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.add";
    command.payload = std::any(std::make_pair(std::string("feature/x"), std::string("")));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

// Regression test (same class as GitBackend_GitTag_NameStartingWithDash_IsRejected).
AISTUDIO_TEST(GitBackend_GitWorktreeAdd_BranchNameStartingWithDash_IsRejected) {
    TempGitRepo repo;
    TempWorktreePath worktree;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.add";
    command.payload = std::any(std::make_pair(std::string("--force"), worktree.String()));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitWorktreeAdd_PathStartingWithDash_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.add";
    command.payload = std::any(std::make_pair(std::string("feature/x"), std::string("--force")));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitWorktreeList_ReturnsMainAndAddedWorktree) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    TempWorktreePath worktree;
    repo.RunGit({"worktree", "add", "-b", "feature/listed", worktree.String()});

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.worktree.list";
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto entries = std::any_cast<std::vector<GitWorktreeEntry>>(result.Value());
    AISTUDIO_EXPECT(entries.size() == 2);
    const bool found_added = std::any_of(entries.begin(), entries.end(), [&](const GitWorktreeEntry& entry) {
        return entry.branch.find("feature/listed") != std::string::npos;
    });
    AISTUDIO_EXPECT(found_added);
}

AISTUDIO_TEST(GitBackend_GitWorktreeRemove_RemovesCleanWorktree) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    TempWorktreePath worktree;
    repo.RunGit({"worktree", "add", "-b", "feature/to-remove", worktree.String()});
    AISTUDIO_EXPECT(fs::exists(worktree.path));

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.remove";
    command.payload = std::any(worktree.String());
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(!fs::exists(worktree.path));
}

// Verifies the design's core safety claim (GitBackend.hpp: "relied on
// exactly like git.checkout above relies on git's own refusal") against a
// real git binary, not just reasoned about.
AISTUDIO_TEST(GitBackend_GitWorktreeRemove_RefusesWhenWorktreeIsDirty) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    TempWorktreePath worktree;
    repo.RunGit({"worktree", "add", "-b", "feature/dirty", worktree.String()});
    {
        std::ofstream out(worktree.path / "uncommitted.txt", std::ios::binary);
        out << "uncommitted content";
    }

    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.remove";
    command.payload = std::any(worktree.String());
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk()); // git itself refuses -- no --force was passed
    AISTUDIO_EXPECT(fs::exists(worktree.path)); // still there, nothing lost
}

AISTUDIO_TEST(GitBackend_GitWorktreeRemove_EmptyPath_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.remove";
    command.payload = std::any(std::string(""));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitWorktreeRemove_PathStartingWithDash_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Command command;
    command.name = "git.worktree.remove";
    command.payload = std::any(std::string("--force"));
    const auto result = backend->Dispatch(command);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitStatus_ReportsUntrackedFile) {
    TempGitRepo repo;
    repo.WriteFile("committed.txt", "hello\n");
    repo.Commit("initial commit");
    repo.WriteFile("untracked.txt", "new stuff\n");

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.status";
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto status = std::any_cast<GitStatusResult>(result.Value());
    const bool has_untracked = std::any_of(status.entries.begin(), status.entries.end(), [](const GitStatusEntry& e) {
        return e.path == "untracked.txt" && e.status_code == "??";
    });
    AISTUDIO_EXPECT(has_untracked);
}

AISTUDIO_TEST(GitBackend_GitDiff_ShowsModifiedContent) {
    TempGitRepo repo;
    repo.WriteFile("a.txt", "line1\n");
    repo.Commit("add a.txt");
    repo.WriteFile("a.txt", "line1\nline2\n");

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.diff";
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto diff = std::any_cast<std::string>(result.Value());
    AISTUDIO_EXPECT(diff.find("+line2") != std::string::npos);
}

AISTUDIO_TEST(GitBackend_GitDiffStaged_ShowsStagedContentNotUnstagedDiff) {
    TempGitRepo repo;
    repo.WriteFile("a.txt", "line1\n");
    repo.Commit("add a.txt");
    repo.WriteFile("a.txt", "line1\nline2\n");
    repo.RunGit({"add", "a.txt"}); // stage it -- no longer shows up in plain "git diff"

    const auto backend = MakeStartedBackend(repo.RootString());

    Query staged_query;
    staged_query.name = "git.diff.staged";
    const auto staged_result = backend->Handle(staged_query);
    AISTUDIO_EXPECT(staged_result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<std::string>(staged_result.Value()).find("+line2") != std::string::npos);

    Query unstaged_query;
    unstaged_query.name = "git.diff";
    const auto unstaged_result = backend->Handle(unstaged_query);
    AISTUDIO_EXPECT(unstaged_result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<std::string>(unstaged_result.Value()).empty());
}

AISTUDIO_TEST(GitBackend_GitDiffHead_CombinesStagedAndUnstagedChanges) {
    TempGitRepo repo;
    repo.WriteFile("a.txt", "line1\n");
    repo.WriteFile("b.txt", "line1\n");
    repo.Commit("initial");
    repo.WriteFile("a.txt", "line1\nline2\n");
    repo.RunGit({"add", "a.txt"}); // staged
    repo.WriteFile("b.txt", "line1\nline3\n"); // left unstaged

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.diff.head";
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto diff = std::any_cast<std::string>(result.Value());
    AISTUDIO_EXPECT(diff.find("+line2") != std::string::npos);
    AISTUDIO_EXPECT(diff.find("+line3") != std::string::npos);
}

AISTUDIO_TEST(GitBackend_GitDiffBranch_ShowsChangesSinceDivergingFromBase) {
    TempGitRepo repo;
    repo.RunGit({"checkout", "-b", "base"}); // explicit name -- don't depend on git's configured default branch name
    repo.WriteFile("a.txt", "line1\n");
    repo.Commit("base commit");
    repo.RunGit({"branch", "feature"});
    // Advance the base branch AFTER the feature branch point -- this
    // change must NOT show up in "feature"'s three-dot diff against
    // base, since three-dot compares against their merge-base, not
    // base's current tip.
    repo.WriteFile("unrelated.txt", "on base only\n");
    repo.Commit("advance base after divergence point");
    repo.RunGit({"checkout", "feature"});
    repo.WriteFile("a.txt", "line1\nfeature change\n");
    repo.Commit("feature commit");

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.diff.branch";
    query.parameters = std::any(std::string("base"));
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto diff = std::any_cast<std::string>(result.Value());
    AISTUDIO_EXPECT(diff.find("+feature change") != std::string::npos);
    AISTUDIO_EXPECT(diff.find("unrelated.txt") == std::string::npos);
}

AISTUDIO_TEST(GitBackend_GitDiffBranch_EmptyBaseBranch_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.diff.branch";
    query.parameters = std::any(std::string(""));
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitDiffBranch_NoParameters_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.diff.branch"; // parameters left empty
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

// Regression test for a real argument-injection bug found during a
// post-merge implementation review: base_branch is concatenated
// directly into a single "git diff" argument ("<base>...HEAD"). Without
// rejecting a leading '-', base_branch="--output=<path>" makes the
// composite argument "--output=<path>...HEAD" -- a REAL git-diff option
// that redirects the diff to an arbitrary file path instead of stdout.
// Confirmed against a real git binary before this fix existed: the file
// was actually created with diff content at the injected path.
AISTUDIO_TEST(GitBackend_GitDiffBranch_BaseBranchStartingWithDash_IsRejected_NotTreatedAsOutputRedirect) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.WriteFile("f.txt", "2\n");

    const fs::path injected_target = repo.root / "injected_output.txt";
    fs::remove(injected_target); // in case a prior run left it (shouldn't happen, but keep the test honest)

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.diff.branch";
    query.parameters = std::any(std::string("--output=") + injected_target.string());
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);

    // The injected path (give or take the literal "...HEAD" suffix the
    // real exploit would append) must never have been written.
    bool any_injected_file_exists = false;
    for (const auto& entry : fs::directory_iterator(repo.root)) {
        if (entry.path().filename().string().rfind("injected_output.txt", 0) == 0) {
            any_injected_file_exists = true;
        }
    }
    AISTUDIO_EXPECT(!any_injected_file_exists);
}

AISTUDIO_TEST(GitBackend_GitHistory_ReturnsCommitsMostRecentFirst) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("first");
    repo.WriteFile("f.txt", "2\n");
    repo.Commit("second");
    repo.WriteFile("f.txt", "3\n");
    repo.Commit("third");

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.history";
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto commits = std::any_cast<std::vector<GitCommitEntry>>(result.Value());
    AISTUDIO_EXPECT(commits.size() == 3);
    AISTUDIO_EXPECT(commits[0].subject == "third");
    AISTUDIO_EXPECT(commits[2].subject == "first");
}

AISTUDIO_TEST(GitBackend_GitHistory_MaxCountLimitsResults) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("first");
    repo.WriteFile("f.txt", "2\n");
    repo.Commit("second");
    repo.WriteFile("f.txt", "3\n");
    repo.Commit("third");

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.history";
    query.parameters = std::any(2);
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    const auto commits = std::any_cast<std::vector<GitCommitEntry>>(result.Value());
    AISTUDIO_EXPECT(commits.size() == 2);
}

AISTUDIO_TEST(GitBackend_GitLogSymbol_FindsCommitsThatChangedTheSymbolsOccurrenceCount) {
    TempGitRepo repo;
    repo.WriteFile("f.cpp", "void Foo() {\n}\n");
    repo.Commit("add Foo"); // introduces one occurrence of "Foo"
    repo.WriteFile("f.cpp", "void Foo() {\n}\nvoid Bar() {\n}\n");
    repo.Commit("add Bar"); // doesn't add/remove any line containing "Foo"
    repo.WriteFile("f.cpp", "void Foo() {\n}\nvoid Bar() {\n}\nvoid FooHelper() {\n}\n");
    repo.Commit("add FooHelper"); // adds a second occurrence of the substring "Foo"

    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.log.symbol";
    query.parameters = std::any(std::string("Foo"));
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(result.IsOk());

    // -S is pickaxe: only commits that changed how many times "Foo"
    // occurs are matched -- "add Bar" never touches a line containing
    // "Foo", so it correctly does NOT appear.
    const auto commits = std::any_cast<std::vector<GitCommitEntry>>(result.Value());
    AISTUDIO_EXPECT(commits.size() == 2);
    AISTUDIO_EXPECT(commits[0].subject == "add FooHelper"); // most recent first
    AISTUDIO_EXPECT(commits[1].subject == "add Foo");
}

AISTUDIO_TEST(GitBackend_GitLogSymbol_EmptySymbolName_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.log.symbol";
    query.parameters = std::any(std::string(""));
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitShow_ReturnsCommitDiff) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.WriteFile("f.txt", "1\n2\n");
    repo.Commit("second");

    const auto backend = MakeStartedBackend(repo.RootString());

    Query history_query;
    history_query.name = "git.history";
    const auto history_result = backend->Handle(history_query);
    const auto commits = std::any_cast<std::vector<GitCommitEntry>>(history_result.Value());
    AISTUDIO_EXPECT(!commits.empty());

    Query show_query;
    show_query.name = "git.show";
    show_query.parameters = std::any(commits[0].sha);
    const auto result = backend->Handle(show_query);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<std::string>(result.Value()).find("+2") != std::string::npos);
}

AISTUDIO_TEST(GitBackend_GitShow_NonHexSha_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.show";
    query.parameters = std::any(std::string("--upload-pack=evil")); // argument-injection shape, not a sha
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_GitShow_TooShortSha_IsRejected) {
    TempGitRepo repo;
    const auto backend = MakeStartedBackend(repo.RootString());
    Query query;
    query.name = "git.show";
    query.parameters = std::any(std::string("abc"));
    const auto result = backend->Handle(query);
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(GitBackend_ProvideContext_MessageMatch_ReturnsCommitDiff) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("unrelated commit");
    repo.WriteFile("f.txt", "2\n");
    repo.Commit("fix rendering bug for WIDGETFOO123");

    const auto backend = MakeStartedBackend(repo.RootString());
    const auto items = backend->ProvideContext("WIDGETFOO123");
    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].content.find("fix rendering bug for WIDGETFOO123") != std::string::npos);
}

// docs/MASTER_SPEC.md #28 "Commit Knowledge"'s own example: an error
// description ("Weapon nullptr") may never appear in a fix commit's
// MESSAGE ("fix crash") -- only in the CODE that commit touched
// ("WeaponComponent"). `--grep` alone would miss this entirely; the
// `-S` pickaxe search added alongside it is what catches it.
AISTUDIO_TEST(GitBackend_ProvideContext_ContentOnlyMatch_FoundViaPickaxe) {
    TempGitRepo repo;
    repo.WriteFile("weapon.cpp", "void Fire() {\n}\n");
    repo.Commit("initial");
    repo.WriteFile("weapon.cpp", "void Fire() {\n    InitializeWeaponComponent();\n}\n");
    repo.Commit("fix crash"); // message says nothing about WeaponComponent

    const auto backend = MakeStartedBackend(repo.RootString());
    const auto items = backend->ProvideContext("WeaponComponent");
    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].content.find("InitializeWeaponComponent") != std::string::npos);
}

AISTUDIO_TEST(GitBackend_ProvideContext_MatchesBothMessageAndContent_IsNotDuplicated) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");
    repo.WriteFile("f.txt", "2 UNIQUETOKEN99\n");
    repo.Commit("fix UNIQUETOKEN99 issue"); // matches both --grep and -S

    const auto backend = MakeStartedBackend(repo.RootString());
    const auto items = backend->ProvideContext("UNIQUETOKEN99");
    AISTUDIO_EXPECT(items.size() == 1); // deduplicated, not returned twice
}

AISTUDIO_TEST(GitBackend_ProvideContext_NoMatches_ReturnsEmpty) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("initial");

    const auto backend = MakeStartedBackend(repo.RootString());
    const auto items = backend->ProvideContext("NoSuchTermAnywhere");
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_BackendRegistryOnly_SurfacesGitCommitMatchingIntent) {
    TempGitRepo repo;
    repo.WriteFile("f.txt", "1\n");
    repo.Commit("unrelated commit");
    repo.WriteFile("f.txt", "2\n");
    repo.Commit("fix WIDGETFOO123 rendering bug");

    const auto backend = MakeStartedBackend(repo.RootString());

    BackendRegistry registry;
    registry.Register(backend);

    // A firewall is deliberately set here (unlike a plain-vanilla setup)
    // -- a real bootstrap (HTTP or MCP mode) always passes one, and a
    // colon in GitBackend's id once made Sandbox reject it as escaping
    // root, silently dropping every Git-provided item; this is the
    // regression guard for that (see Sandbox_Check_
    // ColonContainingNonPathId_IsRejected_KnownWindowsLimitation in
    // test_sandbox.cpp for the underlying Windows quirk).
    const Sandbox firewall(repo.RootString());
    ContextRetriever::Options options;
    options.backend_registry = &registry;
    options.firewall = &firewall;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("WIDGETFOO123");
    const bool has_git_item = std::any_of(items.begin(), items.end(), [](const ContextItem& item) {
        return item.source == ContextSourceKind::GitDiff && item.id.rfind("git/commit/", 0) == 0;
    });
    AISTUDIO_EXPECT(has_git_item);
}
