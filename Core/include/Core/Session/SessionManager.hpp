#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Session/CliProfile.hpp"
#include "Core/Session/ICliAdapter.hpp"
#include "Core/Session/ResourceLeaseLedger.hpp"
#include "Core/Session/Session.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// In-memory session registry (docs/ROADMAP.md 13-2), mirroring TaskQueue's
// own shape: synchronous, no background thread of its own -- a caller
// (GUI frame loop, a test) calls PollAll() on whatever cadence it likes,
// same "caller decides when" design as TaskQueue::CheckTimeouts().
//
// Owns each session's ICliAdapter (process supervision); Session itself
// (Session.hpp) stays plain persisted data with no live handle in it, so
// SessionRepository can round-trip it without knowing about adapters at
// all.
//
// State transitions this class makes on its own are limited to what's
// honestly observable through ICliAdapter (AGENT.md #15): CreateSession
// sets Starting; a successful Start() moves to Running; PollAll()
// detects the adapter's process no longer running and moves to
// Terminated (explicit Stop() call, or ExitCode()==0) or Failed
// (ExitCode() != 0). WaitingForInput/WaitingForApproval are never set by
// PollAll() itself -- SetState() exists for a future adapter that can
// actually detect those from structured output to report them.
class SessionManager {
public:
    // `resource_lease_ledger`, when non-null, is released for a session
    // (ReleaseAllForSession()) the moment that session reaches a terminal
    // state -- whether via an explicit Stop() or PollAll() observing the
    // process exit on its own (docs/ROADMAP.md 13-5's own design note:
    // "ReleaseAllForSession()...SessionManager::PollAll()がセッションの
    // 終了を検出した際にこれを呼ぶことを想定した設計だが、その配線自体
    // は今回行っていない" -- this constructor is that wiring). Default
    // nullptr keeps every existing no-argument SessionManager() call site
    // unchanged.
    explicit SessionManager(ResourceLeaseLedger* resource_lease_ledger = nullptr);

    // Takes ownership of `adapter` and starts it via `profile`. A failed
    // Start() is NOT a Result-level failure -- it still returns Ok with
    // the registered session's state set to Failed (matching
    // ProcessRunner's own "a launch that fails to start is a normal,
    // reportable outcome, not RunProcess's own failure" convention; see
    // SessionManager_CreateSession_FailedStart_IsFailedNotAnErrorResult).
    // This only returns Fail() for `session.id` itself being invalid
    // (empty, or already registered) -- checked before Start() is ever
    // called, so the session is never registered in that case.
    Result<Session> CreateSession(Session session, const CliProfile& profile, std::unique_ptr<ICliAdapter> adapter);

    // Explicit, user-initiated stop -- distinct from PollAll() later
    // observing the same adapter's process as no-longer-running, which
    // this marks so that later observation is classified as Terminated
    // rather than treated as an organic exit.
    Result<void> Stop(const std::string& session_id);

    // Re-Start()s the SAME session's SAME adapter in place -- never
    // creates a new session/adapter, so the caller doesn't need a fresh
    // id and (e.g. for a worktree-backed GenericCliAdapter) the process
    // comes back in the same working directory it was stopped in,
    // instead of the caller having to CreateSession() a whole new
    // workspace just to resume. `profile` is supplied again since
    // Session itself never stores one (same reasoning as CreateSession's
    // own profile parameter) -- pass the same CliProfile used to create
    // it. Rejects a session that isn't terminal (Starting/Running/
    // WaitingForInput/WaitingForApproval) -- restarting something that
    // was never stopped is almost certainly a caller bug. A failed
    // Start() is NOT a Result-level failure, matching CreateSession's
    // own "a launch that fails to start is a normal, reportable outcome"
    // convention -- it still returns Ok with state set to Failed.
    Result<void> Restart(const std::string& session_id, const CliProfile& profile);

    // For an adapter that can report a richer state than PollAll() alone
    // ever infers (WaitingForInput/WaitingForApproval; not used by
    // GenericCliAdapter today). Rejects moving into/out of terminal
    // states through here -- PollAll() alone owns those transitions,
    // since only it consults ExitCode().
    Result<void> SetState(const std::string& session_id, SessionState state);

    // Checks every non-terminal session's IsRunning()/ExitCode() and
    // updates state (publishing "SessionStateChanged" on the EventBus for
    // each one that actually changed -- AGENT.md #5, matching this
    // module's own design note in docs/ROADMAP.md). Returns the ids that
    // changed state this call.
    std::vector<std::string> PollAll();

    [[nodiscard]] std::optional<Session> Find(const std::string& session_id) const;
    [[nodiscard]] std::vector<Session> All() const;

    // For sending input / reading output / resizing the session's
    // terminal -- nullptr if no session with this id is registered.
    [[nodiscard]] ICliAdapter* Adapter(const std::string& session_id);

private:
    struct Entry {
        Session session;
        std::unique_ptr<ICliAdapter> adapter;
        bool stop_requested = false;
        // Set while Restart() has called adapter->Start() but not yet
        // reacquired mutex_ to record the outcome -- unlike CreateSession()
        // (whose entry isn't in `sessions_` at all until after Start()
        // returns, so it's unreachable by id), Restart()'s target entry is
        // already there and reachable the whole time. Without this guard,
        // a Stop() landing in that window would call adapter->Stop()
        // concurrently with the in-flight adapter->Start() and re-set
        // stop_requested, potentially misclassifying a later crash as a
        // clean Terminated. Stop() rejects while this is true instead.
        bool restart_in_progress = false;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Entry> sessions_;
    std::vector<std::string> insertion_order_;
    ResourceLeaseLedger* resource_lease_ledger_ = nullptr;
};

} // namespace aistudio::core
