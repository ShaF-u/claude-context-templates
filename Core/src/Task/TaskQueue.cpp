#include "Core/Task/TaskQueue.hpp"

#include <algorithm>

namespace aistudio::core {

namespace {
Error NotFoundError(const std::string& id) {
    return Error{
        .code = ErrorCode::NotFound,
        .message = "task not found: " + id,
        .module = "Core.Task.Queue",
    };
}

Error InvalidStateError(const std::string& id, const std::string& detail) {
    return Error{
        .code = ErrorCode::InvalidArgument,
        .message = "task '" + id + "' invalid state transition: " + detail,
        .module = "Core.Task.Queue",
    };
}
} // namespace

Result<void> TaskQueue::Enqueue(Task task) {
    std::lock_guard lock(mutex_);
    if (tasks_.contains(task.id)) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "task already enqueued: " + task.id,
            .module = "Core.Task.Queue",
        });
    }
    const auto id = task.id;
    task.state = TaskState::Pending;
    task.progress = 0.0;
    tasks_.emplace(id, std::move(task));
    insertion_order_.push_back(id);
    return Result<void>::Ok();
}

bool TaskQueue::DependenciesSatisfied(const Task& task) const {
    for (const auto& dep_id : task.depends_on) {
        const auto it = tasks_.find(dep_id);
        if (it == tasks_.end() || it->second.state != TaskState::Completed) {
            return false;
        }
    }
    return true;
}

std::vector<Task> TaskQueue::Ready() const {
    std::lock_guard lock(mutex_);
    std::vector<Task> result;
    for (const auto& id : insertion_order_) {
        const auto& task = tasks_.at(id);
        if (task.state == TaskState::Pending && DependenciesSatisfied(task)) {
            result.push_back(task);
        }
    }
    return result;
}

Result<void> TaskQueue::Start(const std::string& id, std::int64_t started_at) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    Task& task = it->second;
    if (task.state != TaskState::Pending) {
        return Result<void>::Fail(InvalidStateError(id, "expected Pending"));
    }
    if (!DependenciesSatisfied(task)) {
        return Result<void>::Fail(InvalidStateError(id, "dependencies not satisfied"));
    }
    task.state = TaskState::Running;
    task.started_at = started_at;
    return Result<void>::Ok();
}

Result<void> TaskQueue::Pause(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.state != TaskState::Running) {
        return Result<void>::Fail(InvalidStateError(id, "expected Running"));
    }
    it->second.state = TaskState::Paused;
    return Result<void>::Ok();
}

Result<void> TaskQueue::Resume(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.state != TaskState::Paused) {
        return Result<void>::Fail(InvalidStateError(id, "expected Paused"));
    }
    it->second.state = TaskState::Running;
    return Result<void>::Ok();
}

Result<void> TaskQueue::ReportProgress(const std::string& id, double progress) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.state != TaskState::Running) {
        return Result<void>::Fail(InvalidStateError(id, "expected Running"));
    }
    it->second.progress = std::clamp(progress, 0.0, 1.0);
    return Result<void>::Ok();
}

Result<void> TaskQueue::Complete(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.state != TaskState::Running) {
        return Result<void>::Fail(InvalidStateError(id, "expected Running"));
    }
    it->second.state = TaskState::Completed;
    it->second.progress = 1.0;
    return Result<void>::Ok();
}

Result<void> TaskQueue::Fail(const std::string& id, Error error) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.state != TaskState::Running) {
        return Result<void>::Fail(InvalidStateError(id, "expected Running"));
    }
    it->second.state = TaskState::Failed;
    last_errors_[id] = std::move(error);
    return Result<void>::Ok();
}

Result<void> TaskQueue::Retry(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    Task& task = it->second;
    if (task.state != TaskState::Failed) {
        return Result<void>::Fail(InvalidStateError(id, "expected Failed"));
    }
    if (task.retry_count >= task.max_retries) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::PermissionDenied,
            .message = "task '" + id + "' exceeded max_retries (" + std::to_string(task.max_retries) + ")",
            .module = "Core.Task.Queue",
            .retryable = false,
        });
    }
    task.retry_count += 1;
    task.state = TaskState::Pending;
    task.progress = 0.0;
    return Result<void>::Ok();
}

Result<void> TaskQueue::Cancel(const std::string& id) {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.IsTerminal()) {
        return Result<void>::Fail(InvalidStateError(id, "task already terminal"));
    }
    it->second.state = TaskState::Cancelled;
    return Result<void>::Ok();
}

std::vector<std::string> TaskQueue::CheckTimeouts(std::int64_t now) {
    std::lock_guard lock(mutex_);
    std::vector<std::string> timed_out;
    for (const auto& id : insertion_order_) {
        Task& task = tasks_.at(id);
        if (task.state != TaskState::Running || task.timeout_seconds <= 0) {
            continue;
        }
        if (now - task.started_at >= task.timeout_seconds) {
            task.state = TaskState::Failed;
            last_errors_[id] = Error{
                .code = ErrorCode::Timeout,
                .message = "task '" + id + "' exceeded timeout of " + std::to_string(task.timeout_seconds) + "s",
                .module = "Core.Task.Queue",
                .retryable = true, // a timeout may just mean "needed more time", not a permanent failure
            };
            timed_out.push_back(id);
        }
    }
    return timed_out;
}

std::optional<Task> TaskQueue::Find(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = tasks_.find(id);
    if (it == tasks_.end()) return std::nullopt;
    return it->second;
}

std::optional<Error> TaskQueue::LastError(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = last_errors_.find(id);
    if (it == last_errors_.end()) return std::nullopt;
    return it->second;
}

std::vector<Task> TaskQueue::All() const {
    std::lock_guard lock(mutex_);
    std::vector<Task> result;
    result.reserve(insertion_order_.size());
    for (const auto& id : insertion_order_) {
        result.push_back(tasks_.at(id));
    }
    return result;
}

} // namespace aistudio::core
