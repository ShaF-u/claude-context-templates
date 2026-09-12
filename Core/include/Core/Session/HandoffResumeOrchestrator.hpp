#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Session/HandoffRecord.hpp"
#include "Core/Session/SessionManager.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-7. 中断後の復旧と二重実行防止":
// "会話再開に対応しないCLIには、引き継ぎ情報（13-6）を使った新規セッ
// ション起動を提供する". Confirmed with the user (2026-09-10): both a
// manual trigger (call ResumeViaHandoff directly) and an opt-in
// automatic trigger (ObservePollResult, gated by Options::
// auto_trigger_enabled) are wanted. auto_trigger_enabled defaults to
// false, matching this codebase's established "a new kind of action
// stays opt-in" convention (mcp.enable_git_write_commands,
// mcp.enable_lsp_rename) -- wiring that boolean to an actual config
// source (aistudio.config) or a GUI toggle is left to whoever wires this
// class into main.cpp/the GUI, same as every other Session-layer piece
// built so far.
//
// Builds a concise prompt from a HandoffRecord's own claimed/verified
// fields, not full verification output -- only command+outcome per
// entry, matching the Context Efficiency Program's own preference for a
// short payload over dumping everything upfront (docs/ROADMAP.md
// "Context Efficiency Program").
class HandoffResumeOrchestrator {
public:
    struct Options {
        bool auto_trigger_enabled = false;
    };

    explicit HandoffResumeOrchestrator(Options options = {});

    // Pure decision, no side effects: should an ended session's task be
    // resumed via a new session seeded with a HandoffRecord? True only
    // when auto_trigger is enabled, `ended_state` is Failed (a session
    // that stopped cleanly -- Terminated -- has nothing to resume), and
    // `capabilities.resume_conversation` is not Supported (a CLI that
    // can resume its own conversation doesn't need this path).
    [[nodiscard]] bool ShouldAutoResume(const CliCapabilities& capabilities, SessionState ended_state) const;

    // The actual action -- usable directly as the manual trigger, and
    // also what ObservePollResult() below calls into. Starts
    // `new_session` via `new_adapter`/`profile` (delegating to
    // `manager.CreateSession()`), then sends BuildHandoffPrompt(record)
    // as the new session's first input. Fails exactly when
    // CreateSession() does. On success, remembers `new_session`'s id so
    // ObservePollResult() never auto-resumes it again if it also ends up
    // Failed -- see that method's own comment for why (this call is the
    // single choke point both the manual and automatic paths go
    // through, so recording here covers both origins).
    Result<Session> ResumeViaHandoff(SessionManager& manager, const HandoffRecord& record, Session new_session,
                                      const CliProfile& profile, std::unique_ptr<ICliAdapter> new_adapter);

    // Automatic driver -- call with SessionManager::PollAll()'s own
    // return value right after calling it. For each session id
    // ShouldAutoResume() approves, calls `find_handoff` to look up that
    // session's HandoffRecord and `make_adapter`/`make_profile` to build
    // a fresh adapter/profile for the same CLI -- this class has no
    // repository dependency or per-CLI adapter factory/registry of its
    // own (neither exists yet in this codebase), the same decoupling
    // ResourceLeaseLedger/OperationLedger keep from their own
    // collaborators. Skips a session silently (no entry in the returned
    // vector) when ShouldAutoResume() is false, `find_handoff` returns
    // nullopt, or the session id was itself produced by an earlier
    // ResumeViaHandoff() call -- without that last check, a CLI/profile
    // combination that keeps failing would cause this to spawn a new
    // resume session every poll cycle forever; capping the chain at one
    // hop is a deliberately conservative default for a first version of
    // an automatic-respawn feature. An automatic path that can't (or
    // shouldn't) act on a session should never surface that as an error.
    //
    // TRUSTS THE CALLER not to pass the same originally-failed id twice
    // across separate calls (e.g. a replayed/stale `changed_session_ids`
    // list, or two independent call sites reacting to the same poll) --
    // the chain-limit guard above only recognizes ids THIS method itself
    // generated, not ids it has already resumed from. Under the intended
    // call pattern (pass PollAll()'s own return value right after
    // calling it) this can't happen, since a terminal session never
    // reappears in a later PollAll() result.
    std::vector<Result<Session>> ObservePollResult(
        SessionManager& manager, const std::vector<std::string>& changed_session_ids,
        const std::function<std::optional<HandoffRecord>(const std::string&)>& find_handoff,
        const std::function<std::unique_ptr<ICliAdapter>()>& make_adapter,
        const std::function<CliProfile(const Session&)>& make_profile);

private:
    Options options_;
    // Guards resume_generated_session_ids_ only -- matches
    // SessionManager/ResourceLeaseLedger/OperationLedger's own
    // convention of being safely callable from whatever thread/cadence
    // the caller uses (a GUI frame loop, a test), even though the only
    // mutable state here is this one set.
    mutable std::mutex mutex_;
    std::unordered_set<std::string> resume_generated_session_ids_;
};

// Concise, claimed/verified-summary-only prompt for a new session to
// pick up where `record` left off -- see the class comment above for
// why full verification output is left out.
[[nodiscard]] std::string BuildHandoffPrompt(const HandoffRecord& record);

} // namespace aistudio::core
