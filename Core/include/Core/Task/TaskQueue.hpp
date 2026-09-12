#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Task/Task.hpp"
#include "Core/Util/Time.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// In-memory task queue with dependency tracking. Intentionally
// synchronous/single-threaded for Phase 1's minimal implementation; a
// worker-pool executor can be layered on top later without changing this
// surface (AGENT.md #14 — minimal implementation first).
//
// Retries are explicit, not automatic: Fail() marks a task Failed, and a
// caller (the future Agent, after diagnosing the failure per
// docs/DEVELOPMENT_PROTOCOL.md #7) decides whether to call Retry().
class TaskQueue {
public:
    Result<void> Enqueue(Task task);

    // Pending tasks whose dependencies have all reached Completed, in
    // insertion order. Does not mutate state — callers still call Start().
    [[nodiscard]] std::vector<Task> Ready() const;

    // `started_at` defaults to real wall-clock time; a caller only passes
    // it explicitly for deterministic testing (mirrors CheckTimeouts()
    // below, and Cache<T>'s injectable NowFn for the same reason).
    Result<void> Start(const std::string& id, std::int64_t started_at = CurrentUnixTimestamp());
    Result<void> Pause(const std::string& id);
    Result<void> Resume(const std::string& id);
    Result<void> ReportProgress(const std::string& id, double progress);
    Result<void> Complete(const std::string& id);
    Result<void> Fail(const std::string& id, Error error);
    Result<void> Retry(const std::string& id);
    Result<void> Cancel(const std::string& id);

    // Transitions every Running task whose elapsed time (now - started_at)
    // has reached its own timeout_seconds (0, the default, means "never
    // times out" and is skipped) to Failed with an ErrorCode::Timeout
    // error — the same effect as calling Fail() on it. Returns the ids
    // that were timed out, in insertion order. TaskQueue has no timer or
    // background thread of its own (matching its "intentionally
    // synchronous" design, see the class comment), so a caller decides
    // when to call this — e.g. poll periodically, or check around other
    // queue operations.
    std::vector<std::string> CheckTimeouts(std::int64_t now = CurrentUnixTimestamp());

    [[nodiscard]] std::optional<Task> Find(const std::string& id) const;
    [[nodiscard]] std::optional<Error> LastError(const std::string& id) const;
    [[nodiscard]] std::vector<Task> All() const;

private:
    [[nodiscard]] bool DependenciesSatisfied(const Task& task) const;

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Task> tasks_;
    std::vector<std::string> insertion_order_;
    std::unordered_map<std::string, Error> last_errors_;
};

} // namespace aistudio::core
