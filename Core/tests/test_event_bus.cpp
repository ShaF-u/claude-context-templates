#include "test_framework.hpp"
#include "Core/Event/EventBus.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(EventBus_Publish_DeliversPayloadToPlainSubscriber) {
    int received = -1;
    const auto id = EventBus::Instance().Subscribe("EventBusTest.Plain", [&](const std::any& payload) {
        received = std::any_cast<int>(payload);
    });

    EventBus::Instance().Publish("EventBusTest.Plain", 42);

    EventBus::Instance().Unsubscribe("EventBusTest.Plain", id);
    AISTUDIO_EXPECT(received == 42);
}

AISTUDIO_TEST(EventBus_PublishEvent_DeliversFullEventToEventSubscriber) {
    Event received;
    const auto id = EventBus::Instance().SubscribeEvent(
        "EventBusTest.Full", [&](const Event& event) { received = event; });

    Event event;
    event.name = "EventBusTest.Full";
    event.correlation_id = "corr-123";
    event.source = "Core.Test";
    event.request_id = "req-456";
    event.payload = std::string("hello");
    EventBus::Instance().Publish(event);

    EventBus::Instance().Unsubscribe("EventBusTest.Full", id);
    AISTUDIO_EXPECT(received.correlation_id == "corr-123");
    AISTUDIO_EXPECT(received.source == "Core.Test");
    AISTUDIO_EXPECT(received.request_id == "req-456");
    AISTUDIO_EXPECT(std::any_cast<std::string>(received.payload) == "hello");
}

AISTUDIO_TEST(EventBus_PublishNameAndPayload_StillWorksForPlainSubscriber) {
    // The existing Publish(name, payload) overload must keep behaving
    // exactly as before -- it's now implemented in terms of Publish(Event)
    // internally, so this proves that didn't change observable behavior.
    std::string received;
    const auto id = EventBus::Instance().Subscribe("EventBusTest.Compat", [&](const std::any& payload) {
        received = std::any_cast<std::string>(payload);
    });

    EventBus::Instance().Publish("EventBusTest.Compat", std::string("unchanged"));

    EventBus::Instance().Unsubscribe("EventBusTest.Compat", id);
    AISTUDIO_EXPECT(received == "unchanged");
}

AISTUDIO_TEST(EventBus_PublishEvent_PlainSubscriberOnlySeesPayload_NotCorrelationId) {
    // A plain EventHandler (Subscribe()) has no way to see correlation_id/
    // source/request_id even when the publisher used Publish(Event) --
    // it should behave identically to a bare Publish(name, payload).
    int received = -1;
    const auto id = EventBus::Instance().Subscribe("EventBusTest.PlainSeesPayloadOnly",
                                                      [&](const std::any& payload) { received = std::any_cast<int>(payload); });

    Event event;
    event.name = "EventBusTest.PlainSeesPayloadOnly";
    event.correlation_id = "should-not-be-visible-here";
    event.payload = 7;
    EventBus::Instance().Publish(event);

    EventBus::Instance().Unsubscribe("EventBusTest.PlainSeesPayloadOnly", id);
    AISTUDIO_EXPECT(received == 7);
}

AISTUDIO_TEST(EventBus_MixedSubscribers_BothStylesReceiveTheirOwnShape) {
    int plain_received = -1;
    std::string full_received_correlation_id;
    const auto plain_id = EventBus::Instance().Subscribe(
        "EventBusTest.Mixed", [&](const std::any& payload) { plain_received = std::any_cast<int>(payload); });
    const auto full_id = EventBus::Instance().SubscribeEvent(
        "EventBusTest.Mixed", [&](const Event& event) { full_received_correlation_id = event.correlation_id; });

    Event event;
    event.name = "EventBusTest.Mixed";
    event.correlation_id = "corr-mixed";
    event.payload = 99;
    EventBus::Instance().Publish(event);

    EventBus::Instance().Unsubscribe("EventBusTest.Mixed", plain_id);
    EventBus::Instance().Unsubscribe("EventBusTest.Mixed", full_id);
    AISTUDIO_EXPECT(plain_received == 99);
    AISTUDIO_EXPECT(full_received_correlation_id == "corr-mixed");
}

AISTUDIO_TEST(EventBus_Unsubscribe_StopsFullEventHandlerFromReceiving) {
    int call_count = 0;
    const auto id =
        EventBus::Instance().SubscribeEvent("EventBusTest.Unsub", [&](const Event&) { ++call_count; });
    EventBus::Instance().Unsubscribe("EventBusTest.Unsub", id);

    Event event;
    event.name = "EventBusTest.Unsub";
    EventBus::Instance().Publish(event);

    AISTUDIO_EXPECT(call_count == 0);
}

AISTUDIO_TEST(EventBus_PublishEvent_NoSubscribers_DoesNotCrash) {
    Event event;
    event.name = "EventBusTest.NoSubscribers";
    EventBus::Instance().Publish(event); // must not crash/throw
    AISTUDIO_EXPECT(true);
}
