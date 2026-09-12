#include "Core/Git/GitBackend.hpp"

#include "Core/Backend/BackendFactoryRegistry.hpp"
#include "Core/Util/ProcessRunner.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <any>
#include <cctype>
#include <unordered_set>
#include <utility>

namespace aistudio::core {

namespace {

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        if (newline == std::string::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, newline - start));
        start = newline + 1;
    }
    return lines;
}

// `git status --porcelain=v1 --branch`'s first line is "## <branch>..." (or
// just "## <branch>" with no upstream, or "## HEAD (no branch)" detached) --
// everything up to the first "..." or space is the branch name.
std::string ExtractBranchName(const std::string& branch_line) {
    std::string rest = branch_line.substr(3); // skip "## "
    const std::size_t ellipsis = rest.find("...");
    if (ellipsis != std::string::npos) {
        return rest.substr(0, ellipsis);
    }
    const std::size_t space = rest.find(' ');
    return space != std::string::npos ? rest.substr(0, space) : rest;
}

GitStatusResult ParseStatusOutput(const std::string& output) {
    GitStatusResult status;
    for (const auto& line : SplitLines(output)) {
        if (line.empty()) {
            continue;
        }
        if (line.rfind("## ", 0) == 0) {
            status.branch = ExtractBranchName(line);
            continue;
        }
        if (line.size() < 4) {
            continue; // shorter than "XY p" -- not a well-formed porcelain entry
        }
        status.entries.push_back(GitStatusEntry{line.substr(3), line.substr(0, 2)});
    }
    return status;
}

// Splits each line of `git log --pretty=format:%H<US>%ad<US>%s` output
// (US = ASCII Unit Separator, 0x1F) into sha/date/subject. A printable
// delimiter like '|' risks colliding with real commit message content;
// 0x1F effectively never appears in one.
std::vector<GitCommitEntry> ParseHistoryOutput(const std::string& output) {
    std::vector<GitCommitEntry> commits;
    for (const auto& line : SplitLines(output)) {
        if (line.empty()) {
            continue;
        }
        const std::size_t first_sep = line.find('\x1f');
        const std::size_t second_sep = first_sep == std::string::npos ? std::string::npos : line.find('\x1f', first_sep + 1);
        if (first_sep == std::string::npos || second_sep == std::string::npos) {
            continue; // malformed line -- skip rather than fail the whole batch
        }
        GitCommitEntry commit;
        commit.sha = line.substr(0, first_sep);
        commit.date = line.substr(first_sep + 1, second_sep - first_sep - 1);
        commit.subject = line.substr(second_sep + 1);
        commits.push_back(std::move(commit));
    }
    return commits;
}

// `git worktree list --porcelain` emits one block per worktree, blank-line
// separated: "worktree <path>", "HEAD <sha>", then either "branch <ref>"
// or the bare marker "detached". Unlike `git status --porcelain`, blocks
// (not lines) are the record boundary, so this can't reuse SplitLines()
// the way ParseStatusOutput()/ParseHistoryOutput() do.
std::vector<GitWorktreeEntry> ParseWorktreeListOutput(const std::string& output) {
    std::vector<GitWorktreeEntry> entries;
    GitWorktreeEntry current;
    bool has_current = false;
    for (const auto& line : SplitLines(output)) {
        if (line.empty()) {
            if (has_current) {
                entries.push_back(std::move(current));
                current = GitWorktreeEntry{};
                has_current = false;
            }
            continue;
        }
        if (line.rfind("worktree ", 0) == 0) {
            current.path = line.substr(9);
            has_current = true;
        } else if (line.rfind("HEAD ", 0) == 0) {
            current.head_sha = line.substr(5);
        } else if (line.rfind("branch ", 0) == 0) {
            current.branch = line.substr(7);
        }
        // "detached", "bare", "locked"/"prunable" (+ optional reason) are
        // recognized-but-unused -- current.branch just stays empty, which
        // is already this struct's documented "detached" signal.
    }
    if (has_current) {
        entries.push_back(std::move(current));
    }
    return entries;
}

// A commit sha (full or abbreviated) is always plain hex -- never
// contains '-', so requiring this shape structurally rules out the
// value being misread as a git option when passed as a bare positional
// argument (see GitBackend.hpp's own comment on "git.show").
bool IsPlausibleGitSha(const std::string& value) {
    if (value.size() < 4 || value.size() > 40) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// True if `value` would be parsed by git as an OPTION rather than as the
// plain ref/branch name it's meant to be, when passed as a single bare
// positional argument (no preceding disambiguating "--", and not the
// value slot of a flag like "-m" -- git only ever reinterprets a token
// that itself starts with '-'). Ref names starting with '-' are already
// rejected by git's own naming rules (`git check-ref-format`), so this
// costs no legitimate branch/tag name. Found by actually testing this
// class of input against a real git binary rather than assuming the
// single-token shape was safe: `git checkout -f` (no other argument)
// silently discards uncommitted changes -- i.e. a caller passing
// branch_name="-f" to "git.checkout" defeats this Backend's documented
// "never forces" guarantee entirely, and `git diff --output=<path>...`
// (via "git.diff.branch"'s base_branch) writes the diff to an arbitrary
// file path of the caller's choosing.
bool LooksLikeGitOption(const std::string& value) { return !value.empty() && value.front() == '-'; }

Error GitCommandFailed(const std::string& command, const ProcessResult& result) {
    return Error{
        .code = ErrorCode::Internal,
        .message = command + " exited with code " + std::to_string(result.exit_code) + ": " + result.output,
        .module = "Core.Git.Backend",
    };
}

} // namespace

Result<void> GitBackend::Start() {
    const auto result = RunProcess(git_executable_, {"--version"}, root_);
    available_ = result && result.Value().exit_code == 0;
    return Result<void>::Ok();
}

Result<void> GitBackend::Configure(const Config& config) {
    root_ = config.GetOr("backend.core.git.root", config.GetOr("project.root", "."));
    git_executable_ = config.GetOr("backend.core.git.executable", "git");
    return Result<void>::Ok();
}

CommandResult GitBackend::Dispatch(const Command& command) {
    if (!available_) {
        return CommandResult::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "git executable is not available",
            .module = "Core.Git.Backend",
        });
    }

    if (command.name == "git.commit") {
        if (command.payload.type() != typeid(std::string) ||
            std::any_cast<const std::string&>(command.payload).empty()) {
            return CommandResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.commit requires a non-empty std::string commit message",
                .module = "Core.Git.Backend",
            });
        }
        const auto& message = std::any_cast<const std::string&>(command.payload);

        const auto add_result = RunProcess(git_executable_, {"add", "-A"}, root_);
        if (!add_result) {
            return CommandResult::Fail(add_result.Err());
        }
        if (add_result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git add", add_result.Value()));
        }

        const auto commit_result = RunProcess(git_executable_, {"commit", "-m", message}, root_);
        if (!commit_result) {
            return CommandResult::Fail(commit_result.Err());
        }
        if (commit_result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git commit", commit_result.Value()));
        }
        return CommandResult::Ok(std::any(commit_result.Value().output));
    }

    if (command.name == "git.branch") {
        if (command.payload.type() != typeid(std::string) ||
            std::any_cast<const std::string&>(command.payload).empty() ||
            LooksLikeGitOption(std::any_cast<const std::string&>(command.payload))) {
            return CommandResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.branch requires a non-empty std::string branch name that doesn't start with '-'",
                .module = "Core.Git.Backend",
            });
        }
        const auto& branch_name = std::any_cast<const std::string&>(command.payload);

        const auto result = RunProcess(git_executable_, {"branch", branch_name}, root_);
        if (!result) {
            return CommandResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git branch", result.Value()));
        }
        return CommandResult::Ok(std::any(result.Value().output));
    }

    if (command.name == "git.stash") {
        std::vector<std::string> args{"stash", "push"};
        if (command.payload.type() == typeid(std::string)) {
            const auto& message = std::any_cast<const std::string&>(command.payload);
            if (!message.empty()) {
                args.emplace_back("-m");
                args.push_back(message);
            }
        }

        const auto result = RunProcess(git_executable_, args, root_);
        if (!result) {
            return CommandResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git stash", result.Value()));
        }
        return CommandResult::Ok(std::any(result.Value().output));
    }

    if (command.name == "git.checkout") {
        if (command.payload.type() != typeid(std::string) ||
            std::any_cast<const std::string&>(command.payload).empty() ||
            LooksLikeGitOption(std::any_cast<const std::string&>(command.payload))) {
            return CommandResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.checkout requires a non-empty std::string branch name that doesn't start with '-'",
                .module = "Core.Git.Backend",
            });
        }
        const auto& branch_name = std::any_cast<const std::string&>(command.payload);

        // Deliberately no "-b" (that's git.branch's job -- keeps
        // "create" and "switch" as two separate, individually
        // approvable operations) and no "--force": if the checkout
        // would discard uncommitted changes, git's own default refusal
        // is what protects the user here, not anything this Backend
        // adds itself -- which the LooksLikeGitOption() check above is
        // now load-bearing for: without it, branch_name="-f" would BE
        // that "--force" flag, discarding uncommitted work outright
        // (confirmed against a real git binary, not just reasoned about).
        const auto result = RunProcess(git_executable_, {"checkout", branch_name}, root_);
        if (!result) {
            return CommandResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git checkout", result.Value()));
        }
        return CommandResult::Ok(std::any(result.Value().output));
    }

    if (command.name == "git.tag") {
        if (command.payload.type() != typeid(std::string) ||
            std::any_cast<const std::string&>(command.payload).empty() ||
            LooksLikeGitOption(std::any_cast<const std::string&>(command.payload))) {
            return CommandResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.tag requires a non-empty std::string tag name that doesn't start with '-'",
                .module = "Core.Git.Backend",
            });
        }
        const auto& tag_name = std::any_cast<const std::string&>(command.payload);

        // Lightweight tag at HEAD only -- no "-a"/"-m" (annotated tag
        // message), no "-f" (would silently move an existing tag) or
        // "-d" (delete); same minimal, easily-undone shape as
        // "git.branch".
        const auto result = RunProcess(git_executable_, {"tag", tag_name}, root_);
        if (!result) {
            return CommandResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git tag", result.Value()));
        }
        return CommandResult::Ok(std::any(result.Value().output));
    }

    if (command.name == "git.worktree.add") {
        if (command.payload.type() != typeid(std::pair<std::string, std::string>)) {
            return CommandResult::Fail(Error{.code = ErrorCode::InvalidArgument,
                                              .message = "git.worktree.add requires a std::pair<std::string, "
                                                         "std::string> {branch_name, path}",
                                              .module = "Core.Git.Backend"});
        }
        const auto& [branch_name, path] = std::any_cast<const std::pair<std::string, std::string>&>(command.payload);
        if (branch_name.empty() || LooksLikeGitOption(branch_name) || path.empty() || LooksLikeGitOption(path)) {
            return CommandResult::Fail(Error{.code = ErrorCode::InvalidArgument,
                                              .message = "git.worktree.add requires a non-empty branch_name and "
                                                         "path, neither starting with '-'",
                                              .module = "Core.Git.Backend"});
        }

        // -b creates `branch_name` (at HEAD) and the new worktree
        // together -- docs/ROADMAP.md 13-3 wants one worktree + one new
        // branch per session, not general-purpose "attach an existing
        // branch to a new worktree" semantics.
        const auto result = RunProcess(git_executable_, {"worktree", "add", "-b", branch_name, path}, root_);
        if (!result) {
            return CommandResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git worktree add", result.Value()));
        }
        return CommandResult::Ok(std::any(result.Value().output));
    }

    if (command.name == "git.worktree.remove") {
        if (command.payload.type() != typeid(std::string) ||
            std::any_cast<const std::string&>(command.payload).empty() ||
            LooksLikeGitOption(std::any_cast<const std::string&>(command.payload))) {
            return CommandResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.worktree.remove requires a non-empty std::string path that doesn't start with '-'",
                .module = "Core.Git.Backend",
            });
        }
        const auto& path = std::any_cast<const std::string&>(command.payload);

        // No "--force": a worktree with uncommitted changes or untracked
        // files makes git itself refuse this with a "is dirty" error --
        // relied on exactly like "git.checkout" above relies on git's own
        // refusal, not a manual pre-check this Backend invents itself.
        const auto result = RunProcess(git_executable_, {"worktree", "remove", path}, root_);
        if (!result) {
            return CommandResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return CommandResult::Fail(GitCommandFailed("git worktree remove", result.Value()));
        }
        return CommandResult::Ok(std::any(result.Value().output));
    }

    return CommandResult::Fail(Error{
        .code = ErrorCode::NotFound,
        .message = "core.git does not support command: " + command.name,
        .module = "Core.Git.Backend",
    });
}

QueryResult GitBackend::Handle(const Query& query) {
    if (!available_) {
        return QueryResult::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "git executable is not available",
            .module = "Core.Git.Backend",
        });
    }

    if (query.name == "git.status") {
        const auto result = RunProcess(git_executable_, {"status", "--porcelain=v1", "--branch"}, root_);
        if (!result) {
            return QueryResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return QueryResult::Fail(GitCommandFailed("git status", result.Value()));
        }
        return QueryResult::Ok(std::any(ParseStatusOutput(result.Value().output)));
    }

    if (query.name == "git.worktree.list") {
        const auto result = RunProcess(git_executable_, {"worktree", "list", "--porcelain"}, root_);
        if (!result) {
            return QueryResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return QueryResult::Fail(GitCommandFailed("git worktree list", result.Value()));
        }
        return QueryResult::Ok(std::any(ParseWorktreeListOutput(result.Value().output)));
    }

    if (query.name == "git.diff" || query.name == "git.diff.staged" || query.name == "git.diff.head") {
        std::vector<std::string> args{"diff"};
        if (query.name == "git.diff.staged") {
            args.emplace_back("--cached"); // index vs HEAD -- staged changes only
        } else if (query.name == "git.diff.head") {
            args.emplace_back("HEAD"); // working tree vs HEAD -- staged + unstaged combined
        }
        // else "git.diff": working tree vs index -- unstaged changes only (git's own default).
        if (query.parameters.type() == typeid(std::string)) {
            args.emplace_back("--");
            args.push_back(std::any_cast<std::string>(query.parameters));
        }
        const auto result = RunProcess(git_executable_, args, root_);
        if (!result) {
            return QueryResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return QueryResult::Fail(GitCommandFailed(query.name, result.Value()));
        }
        return QueryResult::Ok(std::any(result.Value().output));
    }

    if (query.name == "git.diff.branch") {
        if (query.parameters.type() != typeid(std::string) ||
            std::any_cast<const std::string&>(query.parameters).empty() ||
            LooksLikeGitOption(std::any_cast<const std::string&>(query.parameters))) {
            return QueryResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.diff.branch requires a non-empty std::string base branch name that doesn't start "
                           "with '-'",
                .module = "Core.Git.Backend",
            });
        }
        const auto& base_branch = std::any_cast<const std::string&>(query.parameters);
        // Three-dot notation: diffs HEAD against the merge-base of
        // base_branch and HEAD, not against base_branch's own tip --
        // otherwise changes landed on base_branch AFTER the current
        // branch diverged would show up as (reverse) changes here too,
        // misattributing upstream history to this branch.
        const auto result = RunProcess(git_executable_, {"diff", base_branch + "...HEAD"}, root_);
        if (!result) {
            return QueryResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return QueryResult::Fail(GitCommandFailed("git diff " + base_branch + "...HEAD", result.Value()));
        }
        return QueryResult::Ok(std::any(result.Value().output));
    }

    if (query.name == "git.history") {
        int max_count = 20;
        if (query.parameters.type() == typeid(int)) {
            max_count = std::any_cast<int>(query.parameters);
        }
        const auto result = RunProcess(
            git_executable_, {"log", "--pretty=format:%H\x1f%ad\x1f%s", "--date=short", "-n", std::to_string(max_count)},
            root_);
        if (!result) {
            return QueryResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return QueryResult::Fail(GitCommandFailed("git log", result.Value()));
        }
        return QueryResult::Ok(std::any(ParseHistoryOutput(result.Value().output)));
    }

    if (query.name == "git.log.symbol") {
        if (query.parameters.type() != typeid(std::string) ||
            std::any_cast<const std::string&>(query.parameters).empty()) {
            return QueryResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.log.symbol requires a non-empty std::string symbol name",
                .module = "Core.Git.Backend",
            });
        }
        const auto& symbol_name = std::any_cast<const std::string&>(query.parameters);
        const auto result = RunProcess(
            git_executable_, {"log", "--pretty=format:%H\x1f%ad\x1f%s", "--date=short", "-S" + symbol_name, "-n", "10"},
            root_);
        if (!result) {
            return QueryResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return QueryResult::Fail(GitCommandFailed("git log -S", result.Value()));
        }
        return QueryResult::Ok(std::any(ParseHistoryOutput(result.Value().output)));
    }

    if (query.name == "git.show") {
        if (query.parameters.type() != typeid(std::string) ||
            !IsPlausibleGitSha(std::any_cast<const std::string&>(query.parameters))) {
            return QueryResult::Fail(Error{
                .code = ErrorCode::InvalidArgument,
                .message = "git.show requires a std::string parameter that looks like a commit sha (4-40 hex chars)",
                .module = "Core.Git.Backend",
            });
        }
        const auto& sha = std::any_cast<const std::string&>(query.parameters);
        const auto result = RunProcess(git_executable_, {"show", sha}, root_);
        if (!result) {
            return QueryResult::Fail(result.Err());
        }
        if (result.Value().exit_code != 0) {
            return QueryResult::Fail(GitCommandFailed("git show", result.Value()));
        }
        return QueryResult::Ok(std::any(result.Value().output));
    }

    return QueryResult::Fail(Error{
        .code = ErrorCode::NotFound,
        .message = "core.git does not support query: " + query.name,
        .module = "Core.Git.Backend",
    });
}

std::vector<ContextItem> GitBackend::ProvideContext(const std::string& intent) const {
    std::vector<ContextItem> items;
    if (!available_ || root_.empty() || intent.empty()) {
        return items;
    }

    // Two independent searches, merged: `--grep` (message) misses the
    // docs/MASTER_SPEC.md #28 "Commit Knowledge" example almost by
    // design -- a fix commit's message is often just "fix crash", not
    // the symbol name that actually crashed ("WeaponComponent"). `-S`
    // (pickaxe) instead searches the DIFF content itself for commits
    // that changed how many times the literal string occurs, catching
    // exactly that case. `-S` defaults to a literal-string match (unlike
    // `-G`, which is always a regex) -- deliberately chosen so `intent`
    // (arbitrary external free text) can't misbehave as a regex.
    const auto message_log = RunProcess(
        git_executable_, {"log", "--pretty=format:%H\x1f%ad\x1f%s", "--date=short", "-i", "--grep=" + intent, "-n", "5"},
        root_);
    const auto content_log =
        RunProcess(git_executable_, {"log", "--pretty=format:%H\x1f%ad\x1f%s", "--date=short", "-S" + intent, "-n", "5"}, root_);

    // Message matches ordered first (a direct textual hit on "what was
    // this commit for" is a stronger signal than "this commit happened
    // to touch code containing the string"); either search failing (e.g.
    // no matches, or intent triggers an argument git rejects) just
    // contributes nothing rather than failing ProvideContext entirely.
    std::vector<GitCommitEntry> matches;
    if (message_log && message_log.Value().exit_code == 0) {
        for (auto& commit : ParseHistoryOutput(message_log.Value().output)) {
            matches.push_back(std::move(commit));
        }
    }
    if (content_log && content_log.Value().exit_code == 0) {
        for (auto& commit : ParseHistoryOutput(content_log.Value().output)) {
            matches.push_back(std::move(commit));
        }
    }

    std::unordered_set<std::string> seen_shas;
    constexpr std::size_t kMaxCommits = 5;
    constexpr std::size_t kMaxDiffChars = 4000;
    int priority = 50;
    for (const auto& commit : matches) {
        if (items.size() >= kMaxCommits) {
            break;
        }
        if (!seen_shas.insert(commit.sha).second) {
            continue; // matched both searches -- keep only the first (message) occurrence
        }
        std::string diff_text;
        if (const auto show_result = RunProcess(git_executable_, {"show", commit.sha}, root_);
            show_result && show_result.Value().exit_code == 0) {
            diff_text = show_result.Value().output;
        }
        if (diff_text.size() > kMaxDiffChars) {
            // Utf8SafeTruncationLength, not a raw resize(kMaxDiffChars):
            // a diff of e.g. this project's own Japanese-comment source
            // can have a multi-byte character straddling the cut point,
            // which would otherwise leave invalid UTF-8 that crashes
            // JSON serialization downstream (docs/ROADMAP.md CE-5).
            diff_text.resize(Utf8SafeTruncationLength(diff_text, kMaxDiffChars));
            diff_text += "\n... [truncated]";
        }

        ContextItem item;
        // "/"-separated, not "git:commit:<sha>" -- defensive practice
        // against Windows' NTFS Alternate Data Stream path syntax
        // ("file:stream"), even though Sandbox itself now handles a
        // colon here correctly too (see test_sandbox.cpp's
        // Sandbox_Check_*_UnderRelativeDotRoot_IsAllowed tests for the
        // real bug this id shape was originally (mis)blamed for).
        item.id = "git/commit/" + commit.sha;
        item.source = ContextSourceKind::GitDiff;
        item.compression = CompressionLevel::Raw;
        item.content = "commit " + commit.sha + "\n" + commit.date + " " + commit.subject + "\n\n" + diff_text;
        item.priority = std::clamp(priority, 25, 50);
        item.estimated_tokens = EstimateTokens(item.content);
        items.push_back(std::move(item));

        priority -= 5;
    }
    return items;
}

AISTUDIO_REGISTER_BACKEND(GitBackend);

} // namespace aistudio::core
