#include "test_framework.hpp"
#include "Core/Security/ApprovalQueue.hpp"

using namespace aistudio::core;

namespace {
Command MakeCommand(std::string name) {
    Command command;
    command.name = std::move(name);
    return command;
}
} // namespace

AISTUDIO_TEST(ApprovalQueue_Submit_AppearsInPending) {
    ApprovalQueue queue;
    const auto id = queue.Submit(MakeCommand("git.push"));

    const auto pending = queue.Pending();
    AISTUDIO_EXPECT(pending.size() == 1);
    AISTUDIO_EXPECT(pending[0].id == id);
    AISTUDIO_EXPECT(pending[0].status == ApprovalStatus::Pending);
    AISTUDIO_EXPECT(pending[0].command.name == "git.push");
}

AISTUDIO_TEST(ApprovalQueue_Approve_ChangesStatusAndClearsPending) {
    ApprovalQueue queue;
    const auto id = queue.Submit(MakeCommand("git.push"));

    AISTUDIO_EXPECT(queue.Approve(id));
    AISTUDIO_EXPECT(queue.Pending().empty());

    const auto found = queue.Find(id);
    AISTUDIO_EXPECT(found.has_value());
    AISTUDIO_EXPECT(found->status == ApprovalStatus::Approved);
}

AISTUDIO_TEST(ApprovalQueue_Reject_ChangesStatus) {
    ApprovalQueue queue;
    const auto id = queue.Submit(MakeCommand("git.push"));

    AISTUDIO_EXPECT(queue.Reject(id));
    const auto found = queue.Find(id);
    AISTUDIO_EXPECT(found.has_value());
    AISTUDIO_EXPECT(found->status == ApprovalStatus::Rejected);
}

AISTUDIO_TEST(ApprovalQueue_Approve_MissingId_Fails) {
    ApprovalQueue queue;
    AISTUDIO_EXPECT(queue.Approve("missing").IsError());
}

AISTUDIO_TEST(ApprovalQueue_Approve_AlreadyResolved_Fails) {
    ApprovalQueue queue;
    const auto id = queue.Submit(MakeCommand("git.push"));
    queue.Approve(id);

    AISTUDIO_EXPECT(queue.Approve(id).IsError());
}

AISTUDIO_TEST(ApprovalQueue_Remove_DeletesRequest) {
    ApprovalQueue queue;
    const auto id = queue.Submit(MakeCommand("git.push"));

    queue.Remove(id);
    AISTUDIO_EXPECT(!queue.Find(id).has_value());
}

AISTUDIO_TEST(ApprovalQueue_Find_MissingId_ReturnsNullopt) {
    ApprovalQueue queue;
    AISTUDIO_EXPECT(!queue.Find("missing").has_value());
}
