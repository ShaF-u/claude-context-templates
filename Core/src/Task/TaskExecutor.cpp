#include "Core/Task/TaskExecutor.hpp"

#include <algorithm>
#include <chrono>
#include <exception>

namespace aistudio::core {

TaskExecutor::TaskExecutor(TaskQueue& queue) : queue_(queue) {}

TaskExecutor::~TaskExecutor() {
    std::vector<std::future<void>> workers;
    {
        std::lock_guard lock(mutex_);
        workers = std::move(workers_);
    }
    // wait() rather than letting the vector's own destructor run each
    // future's dtor in turn -- functionally the same (std::async's
    // future blocks on destruction too), but explicit about why: every
    // job must have actually finished before this destructor returns.
    for (auto& worker : workers) {
        if (worker.valid()) {
            worker.wait();
        }
    }
}

Result<void> TaskExecutor::Submit(Task task, Job job) {
    const std::string id = task.id;
    if (const auto enqueue_result = queue_.Enqueue(std::move(task)); !enqueue_result) {
        return enqueue_result;
    }
    {
        std::lock_guard lock(mutex_);
        jobs_.emplace(id, std::move(job));
    }
    DispatchReady();
    return Result<void>::Ok();
}

void TaskExecutor::DispatchReady() {
    for (const auto& task : queue_.Ready()) {
        std::lock_guard lock(mutex_);
        if (!jobs_.contains(task.id)) {
            continue; // Ready() can surface tasks Enqueue()'d directly, bypassing Submit()
        }
        // Start() is the actual race-free gate (TaskQueue's own mutex):
        // if another thread's DispatchReady() already started this task
        // between Ready() above and here, this call fails and this
        // thread simply doesn't spawn a duplicate worker.
        if (!queue_.Start(task.id)) {
            continue;
        }
        // Prune already-finished workers opportunistically -- bounds
        // this vector's growth without a dedicated reaper thread
        // (AGENT.md #14: only as much machinery as this workload needs;
        // final cleanup in ~TaskExecutor() catches whatever's left).
        workers_.erase(std::remove_if(workers_.begin(), workers_.end(),
                                       [](std::future<void>& f) {
                                           return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
                                       }),
                        workers_.end());
        workers_.push_back(std::async(std::launch::async, &TaskExecutor::RunJob, this, task.id));
    }
}

void TaskExecutor::RunJob(std::string id) {
    Job job;
    {
        std::lock_guard lock(mutex_);
        const auto it = jobs_.find(id);
        if (it == jobs_.end()) {
            return;
        }
        job = it->second;
    }

    Result<void> result = Result<void>::Ok();
    try {
        result = job();
    } catch (const std::exception& e) {
        result = Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = std::string("task job threw: ") + e.what(),
            .module = "Core.Task.Executor",
        });
    } catch (...) {
        result = Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "task job threw a non-std::exception value",
            .module = "Core.Task.Executor",
        });
    }

    {
        std::lock_guard lock(mutex_);
        jobs_.erase(id);
    }

    if (result) {
        (void)queue_.Complete(id);
    } else {
        (void)queue_.Fail(id, result.Err());
    }

    DispatchReady(); // a dependent task may have just become Ready
}

} // namespace aistudio::core
