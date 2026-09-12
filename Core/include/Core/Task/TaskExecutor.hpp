#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Task/Task.hpp"
#include "Core/Task/TaskQueue.hpp"

#include <functional>
#include <future>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// Layers actual background execution on top of TaskQueue's own state
// ledger (AGENT.md #6's Start/Progress/Pause/Resume/Cancel/Complete/
// Failed lifecycle) -- TaskQueue's own class comment already documents
// this as the intended shape ("a worker-pool executor can be layered on
// top later without changing this surface"), so TaskQueue itself is
// unmodified here. docs/ROADMAP.md Phase 13 "着手前に潰すべき前提":
// without this, nothing in this project actually runs work in the
// background and reports Start/Complete/Failed back automatically.
//
// One OS thread per running job via std::async(std::launch::async, ...)
// -- Task carries no notion of a worker-pool size, and this project has
// no concrete workload profile yet to size a pool against (AGENT.md #14,
// don't invent a number nobody's asked for). The expected shape here is
// a dependency graph with a handful of concurrently ready tasks, not a
// high-throughput job queue.
//
// Pause/Resume/Cancel/ReportProgress stay exactly what TaskQueue's own
// class comment already establishes: state transitions a caller/job
// cooperates with. There's no portable way to force-suspend or interrupt
// an arbitrary std::function running on its own thread, so this class
// doesn't pretend to -- a job that wants to be pausable/cancellable
// checks shared state of its own choosing while it runs.
class TaskExecutor {
public:
    // Returning Fail() marks the task Failed with that Error. Throwing
    // is also treated as failure (the exception's what(), or a generic
    // message for a non-std::exception) rather than propagating -- an
    // exception escaping a background thread terminates the whole
    // process, which one job author's bug shouldn't be able to do to
    // every other task this executor is running.
    using Job = std::function<Result<void>()>;

    explicit TaskExecutor(TaskQueue& queue);

    // Waits for every still-running job to finish before returning --
    // an executor going out of scope while jobs are still running
    // silently orphaning them (the way a detached thread would) is a
    // worse default than a bounded wait here (the same reasoning
    // ManagedProcess's own destructor already uses for its background
    // reader thread).
    ~TaskExecutor();

    TaskExecutor(const TaskExecutor&) = delete;
    TaskExecutor& operator=(const TaskExecutor&) = delete;
    TaskExecutor(TaskExecutor&&) = delete;
    TaskExecutor& operator=(TaskExecutor&&) = delete;

    // Enqueues `task` into the underlying TaskQueue and registers `job`
    // to run for it. If `task`'s dependencies are already satisfied,
    // starts running it immediately on a new background thread; if not,
    // `job` runs automatically once a dependency this same TaskExecutor
    // is tracking completes and unblocks it. Fails exactly when
    // TaskQueue::Enqueue() would (e.g. duplicate id) -- `job` is never
    // registered or run in that case.
    Result<void> Submit(Task task, Job job);

private:
    void RunJob(std::string id);
    void DispatchReady();

    TaskQueue& queue_;
    std::mutex mutex_;
    std::unordered_map<std::string, Job> jobs_;
    std::vector<std::future<void>> workers_;
};

} // namespace aistudio::core
