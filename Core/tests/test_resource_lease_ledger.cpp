#include "test_framework.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Session/ResourceLeaseLedger.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_FirstRequest_GrantsImmediately) {
    ResourceLeaseLedger ledger;
    const auto result = ledger.Acquire("build_output", "s1", LeaseMode::Exclusive);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().outcome == LeaseAcquireOutcome::Acquired);
    AISTUDIO_EXPECT(result.Value().blocking_holders.empty());

    const auto holders = ledger.HoldersOf("build_output");
    AISTUDIO_EXPECT(holders.size() == 1);
    AISTUDIO_EXPECT(holders[0].session_id == "s1");
    AISTUDIO_EXPECT(holders[0].mode == LeaseMode::Exclusive);
}

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_MultipleShared_AllGrantImmediately) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Shared).Value().outcome == LeaseAcquireOutcome::Acquired);
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Shared).Value().outcome == LeaseAcquireOutcome::Acquired);
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s3", LeaseMode::Shared).Value().outcome == LeaseAcquireOutcome::Acquired);
    AISTUDIO_EXPECT(ledger.HoldersOf("gpu").size() == 3);
}

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_ExclusiveWhileSharedHeld_Queues) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Shared));
    const auto result = ledger.Acquire("gpu", "s2", LeaseMode::Exclusive);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().outcome == LeaseAcquireOutcome::Queued);
    AISTUDIO_EXPECT(result.Value().blocking_holders.size() == 1);
    AISTUDIO_EXPECT(result.Value().blocking_holders[0].session_id == "s1");
    AISTUDIO_EXPECT(ledger.WaitQueueFor("gpu") == std::vector<std::string>{"s2"});
}

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_SharedWhileExclusiveHeld_Queues) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));
    const auto result = ledger.Acquire("gpu", "s2", LeaseMode::Shared);
    AISTUDIO_EXPECT(result.Value().outcome == LeaseAcquireOutcome::Queued);
}

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_SharedWhileQueueNonEmpty_QueuesToAvoidStarvingExclusive) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Shared));       // holds
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive));   // queued (blocked by s1)
    // s3's Shared request WOULD be compatible with current holders (s1,
    // Shared) in isolation, but must not jump ahead of s2's queued
    // Exclusive request.
    const auto result = ledger.Acquire("gpu", "s3", LeaseMode::Shared);
    AISTUDIO_EXPECT(result.Value().outcome == LeaseAcquireOutcome::Queued);
    AISTUDIO_EXPECT(ledger.WaitQueueFor("gpu") == (std::vector<std::string>{"s2", "s3"}));
}

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_SameSessionAlreadyHolding_Fails) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Shared));
    const auto second = ledger.Acquire("gpu", "s1", LeaseMode::Shared);
    AISTUDIO_EXPECT(second.IsError());
    AISTUDIO_EXPECT(second.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_SameSessionAlreadyQueued_Fails) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive)); // queued
    const auto second = ledger.Acquire("gpu", "s2", LeaseMode::Shared);
    AISTUDIO_EXPECT(second.IsError());
}

AISTUDIO_TEST(ResourceLeaseLedger_Release_PromotesNextExclusiveInQueue) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Shared));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive)); // queued
    AISTUDIO_EXPECT(ledger.Release("gpu", "s1"));

    const auto holders = ledger.HoldersOf("gpu");
    AISTUDIO_EXPECT(holders.size() == 1);
    AISTUDIO_EXPECT(holders[0].session_id == "s2");
    AISTUDIO_EXPECT(holders[0].mode == LeaseMode::Exclusive);
    AISTUDIO_EXPECT(ledger.WaitQueueFor("gpu").empty());
}

AISTUDIO_TEST(ResourceLeaseLedger_Release_PromotesRunOfConsecutiveSharedButStopsAtExclusive) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Shared));    // queued
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s3", LeaseMode::Shared));    // queued
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s4", LeaseMode::Exclusive)); // queued
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s5", LeaseMode::Shared));    // queued (behind s4)

    AISTUDIO_EXPECT(ledger.Release("gpu", "s1"));

    const auto holders = ledger.HoldersOf("gpu");
    AISTUDIO_EXPECT(holders.size() == 2); // s2 and s3 both promoted
    AISTUDIO_EXPECT(holders[0].session_id == "s2");
    AISTUDIO_EXPECT(holders[1].session_id == "s3");
    // s4 (Exclusive) and s5 (behind it) remain queued -- s4 can't be
    // promoted while s2/s3 hold Shared, and s5 can't skip ahead of s4.
    AISTUDIO_EXPECT(ledger.WaitQueueFor("gpu") == (std::vector<std::string>{"s4", "s5"}));
}

AISTUDIO_TEST(ResourceLeaseLedger_Release_LastSharedHolderReleasing_UnblocksQueuedExclusive) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Shared));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive)); // queued behind s1

    AISTUDIO_EXPECT(ledger.Release("gpu", "s1"));
    AISTUDIO_EXPECT(ledger.HoldersOf("gpu").size() == 1);
    AISTUDIO_EXPECT(ledger.HoldersOf("gpu")[0].session_id == "s2");
}

AISTUDIO_TEST(ResourceLeaseLedger_Release_UnknownResource_Fails) {
    ResourceLeaseLedger ledger;
    const auto result = ledger.Release("does_not_exist", "s1");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(ResourceLeaseLedger_Release_SessionNeitherHoldsNorQueued_Fails) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Shared));
    const auto result = ledger.Release("gpu", "s2");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(ResourceLeaseLedger_Release_CancelsQueuedWaitWithoutPromotingIt) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive)); // queued
    AISTUDIO_EXPECT(ledger.Release("gpu", "s2")); // s2 cancels its own wait, never held it

    AISTUDIO_EXPECT(ledger.WaitQueueFor("gpu").empty());
    AISTUDIO_EXPECT(ledger.HoldersOf("gpu").size() == 1); // s1 still holds, untouched
}

AISTUDIO_TEST(ResourceLeaseLedger_ReleaseAllForSession_ReleasesAcrossMultipleResourcesAndPromotesOthers) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("build_output", "s1", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive)); // queued behind s1

    ledger.ReleaseAllForSession("s1");

    AISTUDIO_EXPECT(ledger.HoldersOf("build_output").empty());
    const auto gpu_holders = ledger.HoldersOf("gpu");
    AISTUDIO_EXPECT(gpu_holders.size() == 1);
    AISTUDIO_EXPECT(gpu_holders[0].session_id == "s2"); // promoted
}

AISTUDIO_TEST(ResourceLeaseLedger_HoldersOf_UnknownResource_ReturnsEmpty) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.HoldersOf("does_not_exist").empty());
}

AISTUDIO_TEST(ResourceLeaseLedger_WaitQueueFor_ReturnsSessionIdsInFifoOrder) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s3", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.WaitQueueFor("gpu") == (std::vector<std::string>{"s2", "s3"}));
}

AISTUDIO_TEST(ResourceLeaseLedger_Release_PublishesResourceLeaseAcquiredEvent_OnPromotion) {
    ResourceLeaseAcquiredEvent received;
    bool got_event = false;
    const auto sub_id = EventBus::Instance().Subscribe("ResourceLeaseAcquired", [&](const std::any& payload) {
        received = std::any_cast<ResourceLeaseAcquiredEvent>(payload);
        got_event = true;
    });

    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s2", LeaseMode::Exclusive)); // queued
    AISTUDIO_EXPECT(ledger.Release("gpu", "s1"));

    EventBus::Instance().Unsubscribe("ResourceLeaseAcquired", sub_id);
    AISTUDIO_EXPECT(got_event);
    AISTUDIO_EXPECT(received.resource_name == "gpu");
    AISTUDIO_EXPECT(received.holder.session_id == "s2");
}

AISTUDIO_TEST(ResourceLeaseLedger_Acquire_ImmediateGrant_DoesNotPublishEvent) {
    bool got_event = false;
    const auto sub_id = EventBus::Instance().Subscribe("ResourceLeaseAcquired",
                                                         [&](const std::any&) { got_event = true; });

    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("gpu", "s1", LeaseMode::Exclusive));

    EventBus::Instance().Unsubscribe("ResourceLeaseAcquired", sub_id);
    AISTUDIO_EXPECT(!got_event); // immediate grants are communicated via the Acquire() return value only
}
