#include "test_framework.hpp"
#include "Core/Task/TaskQueue.hpp"

using namespace aistudio::core;

namespace {
Task MakeTask(std::string id, std::vector<std::string> deps = {}) {
    Task task;
    task.id = std::move(id);
    task.name = task.id;
    task.depends_on = std::move(deps);
    task.max_retries = 1;
    return task;
}
} // namespace

AISTUDIO_TEST(TaskQueue_Enqueue_DuplicateId_Fails) {
    TaskQueue queue;
    AISTUDIO_EXPECT(queue.Enqueue(MakeTask("a")));
    AISTUDIO_EXPECT(queue.Enqueue(MakeTask("a")).IsError());
}

AISTUDIO_TEST(TaskQueue_Ready_ExcludesUnsatisfiedDependencies) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));
    queue.Enqueue(MakeTask("b", {"a"}));

    const auto ready = queue.Ready();
    AISTUDIO_EXPECT(ready.size() == 1);
    AISTUDIO_EXPECT(ready[0].id == "a");
}

AISTUDIO_TEST(TaskQueue_Ready_IncludesTaskOnceDependencyCompleted) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));
    queue.Enqueue(MakeTask("b", {"a"}));

    queue.Start("a");
    queue.Complete("a");

    const auto ready = queue.Ready();
    AISTUDIO_EXPECT(ready.size() == 1);
    AISTUDIO_EXPECT(ready[0].id == "b");
}

AISTUDIO_TEST(TaskQueue_Start_WithUnsatisfiedDependency_Fails) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));
    queue.Enqueue(MakeTask("b", {"a"}));

    AISTUDIO_EXPECT(queue.Start("b").IsError());
}

AISTUDIO_TEST(TaskQueue_FullLifecycle_PendingToCompleted) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));

    AISTUDIO_EXPECT(queue.Start("a"));
    AISTUDIO_EXPECT(queue.ReportProgress("a", 0.5));
    AISTUDIO_EXPECT(queue.Find("a")->progress == 0.5);
    AISTUDIO_EXPECT(queue.Complete("a"));

    const auto task = queue.Find("a");
    AISTUDIO_EXPECT(task.has_value());
    AISTUDIO_EXPECT(task->state == TaskState::Completed);
    AISTUDIO_EXPECT(task->progress == 1.0);
}

AISTUDIO_TEST(TaskQueue_PauseResume) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));
    queue.Start("a");

    AISTUDIO_EXPECT(queue.Pause("a"));
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Paused);

    AISTUDIO_EXPECT(queue.Resume("a"));
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Running);
}

AISTUDIO_TEST(TaskQueue_Fail_StoresLastError) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));
    queue.Start("a");

    AISTUDIO_EXPECT(queue.Fail("a", Error{.code = ErrorCode::Internal, .message = "boom"}));
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Failed);
    AISTUDIO_EXPECT(queue.LastError("a")->message == "boom");
}

AISTUDIO_TEST(TaskQueue_Retry_RequeuesUntilMaxRetriesExceeded) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a")); // max_retries = 1

    queue.Start("a");
    queue.Fail("a", Error{.code = ErrorCode::Internal, .message = "boom"});

    AISTUDIO_EXPECT(queue.Retry("a"));
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Pending);
    AISTUDIO_EXPECT(queue.Find("a")->retry_count == 1);

    queue.Start("a");
    queue.Fail("a", Error{.code = ErrorCode::Internal, .message = "boom again"});

    AISTUDIO_EXPECT(queue.Retry("a").IsError());
}

AISTUDIO_TEST(TaskQueue_Cancel_TerminalTask_Fails) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));
    queue.Start("a");
    queue.Complete("a");

    AISTUDIO_EXPECT(queue.Cancel("a").IsError());
}

AISTUDIO_TEST(TaskQueue_Cancel_PendingTask_Succeeds) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));

    AISTUDIO_EXPECT(queue.Cancel("a"));
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Cancelled);
}

AISTUDIO_TEST(TaskQueue_All_ReturnsInInsertionOrder) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));
    queue.Enqueue(MakeTask("b"));
    queue.Enqueue(MakeTask("c"));

    const auto all = queue.All();
    AISTUDIO_EXPECT(all.size() == 3);
    AISTUDIO_EXPECT(all[0].id == "a");
    AISTUDIO_EXPECT(all[1].id == "b");
    AISTUDIO_EXPECT(all[2].id == "c");
}

AISTUDIO_TEST(TaskQueue_Start_RecordsStartedAt) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a"));

    AISTUDIO_EXPECT(queue.Start("a", 1000));
    AISTUDIO_EXPECT(queue.Find("a")->started_at == 1000);
}

AISTUDIO_TEST(TaskQueue_CheckTimeouts_NoTimeoutConfigured_NeverTimesOut) {
    TaskQueue queue;
    queue.Enqueue(MakeTask("a")); // timeout_seconds defaults to 0 ("never")
    queue.Start("a", 1000);

    const auto timed_out = queue.CheckTimeouts(1000000);
    AISTUDIO_EXPECT(timed_out.empty());
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Running);
}

AISTUDIO_TEST(TaskQueue_CheckTimeouts_ElapsedBeyondTimeout_FailsTask) {
    TaskQueue queue;
    auto task = MakeTask("a");
    task.timeout_seconds = 60;
    queue.Enqueue(task);
    queue.Start("a", 1000);

    const auto timed_out = queue.CheckTimeouts(1061); // 61s elapsed >= 60s timeout
    AISTUDIO_EXPECT(timed_out.size() == 1);
    AISTUDIO_EXPECT(timed_out[0] == "a");
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Failed);
    AISTUDIO_EXPECT(queue.LastError("a")->code == ErrorCode::Timeout);
}

AISTUDIO_TEST(TaskQueue_CheckTimeouts_NotYetElapsed_LeavesTaskRunning) {
    TaskQueue queue;
    auto task = MakeTask("a");
    task.timeout_seconds = 60;
    queue.Enqueue(task);
    queue.Start("a", 1000);

    const auto timed_out = queue.CheckTimeouts(1030); // only 30s elapsed
    AISTUDIO_EXPECT(timed_out.empty());
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Running);
}

AISTUDIO_TEST(TaskQueue_CheckTimeouts_IgnoresNonRunningTasks) {
    TaskQueue queue;
    auto task = MakeTask("a");
    task.timeout_seconds = 60;
    queue.Enqueue(task); // still Pending, never Start()'d

    const auto timed_out = queue.CheckTimeouts(1000000);
    AISTUDIO_EXPECT(timed_out.empty());
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Pending);
}

AISTUDIO_TEST(TaskQueue_CheckTimeouts_TimedOutTaskIsRetryable) {
    TaskQueue queue;
    auto task = MakeTask("a");
    task.timeout_seconds = 60;
    queue.Enqueue(task);
    queue.Start("a", 1000);
    queue.CheckTimeouts(1061);

    AISTUDIO_EXPECT(queue.LastError("a")->retryable);
    AISTUDIO_EXPECT(queue.Retry("a")); // max_retries = 1 from MakeTask()
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Pending);
}

AISTUDIO_TEST(TaskQueue_CheckTimeouts_MultipleRunningTasks_OnlyFailsExpiredOnes) {
    TaskQueue queue;
    auto expired = MakeTask("expired");
    expired.timeout_seconds = 60;
    auto healthy = MakeTask("healthy");
    healthy.timeout_seconds = 60;
    queue.Enqueue(expired);
    queue.Enqueue(healthy);
    queue.Start("expired", 1000);
    queue.Start("healthy", 1050);

    const auto timed_out = queue.CheckTimeouts(1061);
    AISTUDIO_EXPECT(timed_out.size() == 1);
    AISTUDIO_EXPECT(timed_out[0] == "expired");
    AISTUDIO_EXPECT(queue.Find("expired")->state == TaskState::Failed);
    AISTUDIO_EXPECT(queue.Find("healthy")->state == TaskState::Running);
}
