#include "test_framework.hpp"
#include "Core/Task/TaskExecutor.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aistudio::core;

namespace {
Task MakeTask(std::string id, std::vector<std::string> deps = {}) {
    Task task;
    task.id = std::move(id);
    task.name = task.id;
    task.depends_on = std::move(deps);
    return task;
}
} // namespace

// docs/ROADMAP.md Phase 13 "着手前に潰すべき前提": TaskQueue is
// intentionally a synchronous state ledger, not an executor -- these
// tests exercise the actual background execution TaskExecutor layers on
// top of it. ~TaskExecutor() joining every worker is used as the test
// synchronization point throughout (Submit(), let the executor go out of
// scope, then assert) rather than a polling loop.

AISTUDIO_TEST(TaskExecutor_Submit_RunsJobAndCompletesTask) {
    TaskQueue queue;
    {
        TaskExecutor executor(queue);
        const auto result = executor.Submit(MakeTask("a"), [] { return Result<void>::Ok(); });
        AISTUDIO_EXPECT(result);
    }
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Completed);
}

AISTUDIO_TEST(TaskExecutor_Submit_JobReturningFail_MarksTaskFailed) {
    TaskQueue queue;
    {
        TaskExecutor executor(queue);
        executor.Submit(MakeTask("a"), [] {
            return Result<void>::Fail(
                Error{.code = ErrorCode::Internal, .message = "deliberate failure", .module = "test"});
        });
    }
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Failed);
    AISTUDIO_EXPECT(queue.LastError("a")->message == "deliberate failure");
}

AISTUDIO_TEST(TaskExecutor_Submit_JobThrows_IsCaughtAndMarksTaskFailedNotCrash) {
    TaskQueue queue;
    {
        TaskExecutor executor(queue);
        executor.Submit(MakeTask("a"), []() -> Result<void> { throw std::runtime_error("boom"); });
    }
    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Failed);
    AISTUDIO_EXPECT(queue.LastError("a")->message.find("boom") != std::string::npos);
}

AISTUDIO_TEST(TaskExecutor_Submit_DependentTask_RunsAutomaticallyAfterDependencyCompletes) {
    TaskQueue queue;
    std::mutex order_mutex;
    std::vector<std::string> order;

    {
        TaskExecutor executor(queue);
        executor.Submit(MakeTask("a"), [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::lock_guard lock(order_mutex);
            order.push_back("a");
            return Result<void>::Ok();
        });
        executor.Submit(MakeTask("b", {"a"}), [&] {
            std::lock_guard lock(order_mutex);
            order.push_back("b");
            return Result<void>::Ok();
        });
    }

    AISTUDIO_EXPECT(queue.Find("a")->state == TaskState::Completed);
    AISTUDIO_EXPECT(queue.Find("b")->state == TaskState::Completed);
    AISTUDIO_EXPECT(order.size() == 2);
    AISTUDIO_EXPECT(order[0] == "a");
    AISTUDIO_EXPECT(order[1] == "b");
}

AISTUDIO_TEST(TaskExecutor_Destructor_WaitsForRunningJobToFinish) {
    TaskQueue queue;
    std::atomic<bool> finished{false};
    {
        TaskExecutor executor(queue);
        executor.Submit(MakeTask("a"), [&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            finished = true;
            return Result<void>::Ok();
        });
        // No explicit wait here -- ~TaskExecutor() below is the thing
        // under test.
    }
    AISTUDIO_EXPECT(finished.load());
}

AISTUDIO_TEST(TaskExecutor_Submit_DuplicateId_FailsAndNeverRunsJob) {
    TaskQueue queue;
    std::atomic<bool> ran{false};
    {
        TaskExecutor executor(queue);
        AISTUDIO_EXPECT(executor.Submit(MakeTask("a"), [&] {
            ran = true;
            return Result<void>::Ok();
        }));
        const auto second = executor.Submit(MakeTask("a"), [&] {
            ran = true;
            return Result<void>::Ok();
        });
        AISTUDIO_EXPECT(second.IsError());
    }
    AISTUDIO_EXPECT(ran.load()); // the first Submit()'s job did run
}

AISTUDIO_TEST(TaskExecutor_Submit_UnsatisfiedDependency_DoesNotRunUntilReady) {
    TaskQueue queue;
    std::atomic<int> b_run_count{0};
    {
        TaskExecutor executor(queue);
        executor.Submit(MakeTask("b", {"a"}), [&] {
            ++b_run_count;
            return Result<void>::Ok();
        });
        // "a" was never submitted/enqueued -- "b" must stay Pending, not
        // run, for as long as its dependency never completes.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        AISTUDIO_EXPECT(queue.Find("b")->state == TaskState::Pending);
        AISTUDIO_EXPECT(b_run_count.load() == 0);
    }
}
