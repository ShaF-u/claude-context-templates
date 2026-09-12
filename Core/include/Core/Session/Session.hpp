#pragma once

#include <cstdint>
#include <string>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-2. 複数セッション管理". Unknown is a real
// state, not a fallback: SessionManager only ever sets the more specific
// states (WaitingForInput/WaitingForApproval) when an adapter actually
// reports them -- GenericCliAdapter never does, so its sessions stay at
// Running the whole time they're alive (AGENT.md #15: this is honest,
// not lazy -- Running here means "process alive, no richer signal", not
// "the AI is actively computing", which this codebase currently has no
// way to detect for any CLI. See CliCapabilities for that same
// Unknown-by-default discipline at the per-CLI level).
enum class SessionState {
    Unknown,
    Starting,
    Running,
    WaitingForInput,
    WaitingForApproval,
    Terminated,
    Failed,
};

[[nodiscard]] std::string ToString(SessionState state);

// Plain persisted record for one CLI-type AI session (docs/ROADMAP.md
// 13-2's SessionId/ProjectId/WorkspaceId/TaskId identification). Holds no
// live process handle -- that's SessionManager::Entry's job; a Session
// read back from SessionRepository after a restart describes history,
// not something with a process still attached to it.
struct Session {
    std::string id;
    std::string project_id;
    std::string workspace_id;
    std::string task_id; // empty if this session isn't tied to a specific Task
    std::string cli_name; // matches the ICliAdapter/CliProfile that ran it
    SessionState state = SessionState::Unknown;

    // Unix seconds; 0 means "not yet reached this point".
    std::int64_t created_at = 0;
    std::int64_t started_at = 0;
    std::int64_t ended_at = 0;

    [[nodiscard]] bool IsTerminal() const {
        return state == SessionState::Terminated || state == SessionState::Failed;
    }
};

} // namespace aistudio::core
