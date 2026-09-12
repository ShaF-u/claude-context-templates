#pragma once

#include "Core/Backend/IBackend.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// A single `git status --porcelain=v1` line, split into its 2-character
// status code (e.g. "M ", "??", "A ") and the path it refers to. Codes are
// passed through verbatim rather than decoded into an enum -- callers that
// need porcelain semantics already know the format.
struct GitStatusEntry {
    std::string path;
    std::string status_code;
};

struct GitStatusResult {
    std::string branch;
    std::vector<GitStatusEntry> entries;
};

struct GitCommitEntry {
    std::string sha;
    std::string date;
    std::string subject;
};

// One `git worktree list --porcelain` entry. `branch` is empty for a
// detached-HEAD worktree (porcelain prints a bare "detached" line
// instead of "branch <ref>" in that case) -- passed through as-is rather
// than substituting a placeholder, since "empty" is already a clear,
// unambiguous signal here.
struct GitWorktreeEntry {
    std::string path;
    std::string head_sha;
    std::string branch;
};

// Phase 6 "Git Backend" (docs/MASTER_SPEC.md #26), doubling as the
// "GitProvider" Context Provider SDK entry (#71) the same way
// FileProviderBackend covers both roles for files -- one IBackend, not
// two. Shells out to the `git` CLI (Core/Util/ProcessRunner.hpp) rather
// than embedding a Git implementation; never touches the shell (no
// cmd /c), so there is no shell-injection surface.
//
// Status/Diff/History/worktree list are reads (Handle()). Commit/
// Branch(create)/Stash/Checkout/Tag/worktree add+remove are writes
// (Dispatch()) -- deliberately the subset that is local-only and easy to
// undo. Pull/Push/Merge/Rebase (anything that touches a remote or
// rewrites history) remain out of scope; Dispatch() rejects any other
// command name (see its own comment).
class GitBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "core.git"; }
    [[nodiscard]] std::string Name() const override { return "Git Backend"; }
    [[nodiscard]] std::string Version() const override { return "0.1.0"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override {
        return {"git.status@1.0.0",       "git.diff@1.0.0",         "git.diff.staged@1.0.0", "git.diff.head@1.0.0",
                "git.diff.branch@1.0.0",  "git.history@1.0.0",      "git.log.symbol@1.0.0",  "git.show@1.0.0",
                "git.commit@1.0.0",       "git.branch@1.0.0",       "git.stash@1.0.0",       "git.checkout@1.0.0",
                "git.tag@1.0.0",          "git.worktree.add@1.0.0", "git.worktree.list@1.0.0",
                "git.worktree.remove@1.0.0", "context.provide.git@1.0.0"};
    }

    // Healthy only once Start() has actually confirmed `git --version`
    // runs -- unlike NullBackend/FileProviderBackend's constant Healthy,
    // this Backend has a real external dependency worth reflecting.
    [[nodiscard]] BackendHealth Health() const override { return available_ ? BackendHealth::Healthy : BackendHealth::Unavailable; }

    // Runs `git --version` to populate Health(); always returns Ok() even
    // if git isn't found -- the Backend object itself isn't broken, it
    // just has degraded capability, which Health() (not lifecycle) is the
    // right place to report (mirrors how BackendHealth and
    // BackendLifecycleState are documented as separate concerns).
    Result<void> Start() override;
    Result<void> Stop() override { return Result<void>::Ok(); }

    // Reads `backend.core.git.root` (falling back to `project.root`, then
    // ".") and `backend.core.git.executable` (falling back to "git",
    // resolved via PATH by CreateProcessW).
    Result<void> Configure(const Config& config) override;

    // "git.commit" (payload: non-empty std::string commit message) --
    // stages ALL working-tree changes (`git add -A`) then commits; no
    // per-file staging control yet. "git.branch" (payload: non-empty
    // std::string new branch name) -- creates the branch, does NOT
    // switch to it. "git.stash" (payload: optional std::string stash
    // message) -- `git stash push`. "git.checkout" (payload: non-empty
    // std::string EXISTING branch name) -- `git checkout <branch>`,
    // deliberately never `-b` (that's "git.branch"'s job) or `--force`
    // (git's own default refusal to discard uncommitted changes that
    // would conflict is relied on, not overridden -- this Command can
    // fail, but can't destroy work). "git.tag" (payload: non-empty
    // std::string tag name) -- `git tag <name>` at HEAD; a lightweight
    // tag only (no annotation/message, no `-f`/`-d`) -- same minimal,
    // local-only, easily-undone shape as "git.branch".
    // "git.worktree.add" (payload: non-empty std::pair<std::string,
    // std::string> {branch_name, path}, neither starting with '-') --
    // `git worktree add -b <branch_name> <path>` (docs/ROADMAP.md 13-3:
    // one worktree + one new branch per session, created together;
    // relative `path` resolves against this Backend's own root, same as
    // every other path parameter here). "git.worktree.remove" (payload:
    // non-empty std::string path, not starting with '-') -- `git worktree
    // remove <path>`, deliberately never `--force`: same "trust git's own
    // refusal to discard a dirty worktree" design as "git.checkout"
    // above, not a manual pre-check this Backend invents itself. Anything
    // else fails with NotFound.
    CommandResult Dispatch(const Command& command) override;

    // "git.status" (no parameters). "git.diff"/"git.diff.staged"/
    // "git.diff.head" each take the same optional std::string
    // parameters (a path to limit the diff to) and differ only in what
    // they compare the working tree/index against: "git.diff" is
    // unstaged changes only (working tree vs index, git's own default);
    // "git.diff.staged" is staged-only (`--cached`, index vs HEAD);
    // "git.diff.head" is everything not yet committed, staged and
    // unstaged combined (working tree vs HEAD) -- the one whose new-side
    // line numbers always match the actual current file content, which
    // is what ImpactAnalyzer::AnalyzeChanges() needs to correlate against
    // SymbolIndex (a file with SOME staged and SOME further unstaged
    // changes has index content that doesn't match the working tree, so
    // "git.diff.staged" alone would report line numbers that don't
    // correspond to anything SymbolIndex actually indexed).
    // "git.diff.branch" (required std::string parameter: base branch
    // name, e.g. "Develop") -- everything the CURRENT branch has changed
    // since it diverged from that base branch (`git diff base...HEAD`,
    // three-dot notation: base vs HEAD relative to their merge-base, the
    // same comparison a GitHub/GitLab pull request diff shows). Unlike
    // "git.diff.head" (uncommitted changes only), this also includes
    // everything already committed on the current branch -- "what would
    // my whole branch/PR change", not just "what's still unsaved".
    // "git.history" (optional int parameters: max commit count, default
    // 20).
    // "git.log.symbol" (required non-empty std::string parameter: a
    // symbol/identifier name) -- past commits whose diff changed how
    // many times that literal string occurs (`git log -S<name>`, same
    // pickaxe mechanism ProvideContext() uses), most recent first, capped
    // at 10. "Similar change search" (docs/ROADMAP.md Git Intelligence):
    // answers "how has this specific piece of code changed before?"
    // "git.show" (required std::string parameter: a commit sha, matched
    // against `^[0-9a-fA-F]{4,40}$` and rejected otherwise) -- that
    // commit's full message + diff (`git show <sha>`). The hex-only
    // validation isn't just a sanity check: an unvalidated sha starting
    // with '-' passed as a bare positional argument could be
    // misinterpreted by git as an option instead of a revision (the same
    // class of argument-injection concern "git.diff"'s path parameter
    // already guards against with `--`, applied here via input
    // validation instead since a git revision has no `--` disambiguation
    // form that survives an option-like prefix).
    // "git.worktree.list" (no parameters) -- every worktree this repo
    // currently has (`git worktree list --porcelain`), as
    // std::vector<GitWorktreeEntry>.
    QueryResult Handle(const Query& query) override;

    // Searches commit subjects for `intent` (`git log --grep`, case
    // insensitive) and returns each match's diff as a
    // ContextSourceKind::GitDiff item (id "git/commit/<sha>" -- "/"-
    // separated rather than "git:commit:<sha>" purely as defensive
    // practice against Windows' NTFS Alternate Data Stream path syntax;
    // see Core/Git/GitBackend.cpp's own comment at the id assignment).
    [[nodiscard]] std::vector<ContextItem> ProvideContext(const std::string& intent) const override;

private:
    std::string root_;
    std::string git_executable_ = "git";
    bool available_ = false;
};

} // namespace aistudio::core
