#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aistudio::core {

enum class TaskState {
    Pending,
    Running,
    Paused,
    Completed,
    Failed,
    Cancelled,
};

// A unit of work the TaskQueue schedules and tracks. Mirrors the
// Start/Progress/Pause/Resume/Cancel/Complete/Failed lifecycle required
// for every long-running process (AGENT.md #6).
struct Task {
    std::string id;
    std::string name;
    TaskState state = TaskState::Pending;

    // 0.0 - 1.0
    double progress = 0.0;

    int retry_count = 0;
    int max_retries = 0;

    // Ids of tasks that must reach Completed before this one becomes Ready.
    std::vector<std::string> depends_on;

    // Unix seconds, set by TaskQueue::Start() when this task transitions
    // to Running (0 until then). Not persisted by TaskRepository — the
    // two aren't wired together yet (TaskQueue is still exercised only by
    // its own tests, docs/ROADMAP.md "Task timeout").
    std::int64_t started_at = 0;
    // Seconds this task may stay Running before TaskQueue::CheckTimeouts()
    // fails it. 0 (the default) means "never times out", matching
    // max_retries' own "0 = no automatic behavior" convention above.
    std::int64_t timeout_seconds = 0;

    [[nodiscard]] bool IsTerminal() const {
        return state == TaskState::Completed || state == TaskState::Failed || state == TaskState::Cancelled;
    }
};

[[nodiscard]] inline std::string ToString(TaskState state) {
    switch (state) {
        case TaskState::Pending: return "Pending";
        case TaskState::Running: return "Running";
        case TaskState::Paused: return "Paused";
        case TaskState::Completed: return "Completed";
        case TaskState::Failed: return "Failed";
        case TaskState::Cancelled: return "Cancelled";
    }
    return "Unknown";
}

} // namespace aistudio::core
