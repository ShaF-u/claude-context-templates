#pragma once

#include "Core/Protocol/Event.hpp"

#include <any>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

using EventHandler = std::function<void(const std::any& payload)>;
// A handler that receives the full Event (request_id/correlation_id/
// source/name, not just its payload) — docs/ROADMAP.md "Correlation ID" /
// "Event Routing". Opt-in and additive: every existing Subscribe()/
// Publish(name, payload) call site keeps working completely unchanged
// (payload-only handlers still only ever see the payload); this exists
// for a caller that actually wants to trace/route by correlation_id,
// which the plain EventHandler above has no way to see.
using FullEventHandler = std::function<void(const Event& event)>;

// Minimal synchronous pub/sub so new features notify state changes
// (FileChanged, TaskStarted, BackendConnected, ...) instead of reaching
// into other modules directly (AGENT.md #5, docs/MASTER_SPEC.md #35).
class EventBus {
public:
    static EventBus& Instance();

    int Subscribe(const std::string& event_name, EventHandler handler);
    // Same subscription list/id space as Subscribe() above (a single
    // Unsubscribe() works for either) — the two differ only in what the
    // handler receives once a matching Publish() fires.
    int SubscribeEvent(const std::string& event_name, FullEventHandler handler);
    void Unsubscribe(const std::string& event_name, int subscription_id);

    void Publish(const std::string& event_name, const std::any& payload = {});
    // Publishes a full Event: FullEventHandler subscribers (SubscribeEvent())
    // receive it as-is (correlation_id/source/request_id included); plain
    // EventHandler subscribers (Subscribe()) still just receive
    // event.payload, exactly as if Publish(event.name, event.payload) had
    // been called instead — mixing both subscription styles on the same
    // event name is fine, each gets what it asked for.
    void Publish(const Event& event);

private:
    EventBus() = default;

    struct Subscription {
        int id;
        // Exactly one of these two is set, depending on which Subscribe
        // overload created this Subscription.
        EventHandler handler;
        FullEventHandler full_handler;
    };

    std::mutex mutex_;
    int next_id_ = 1;
    std::unordered_map<std::string, std::vector<Subscription>> subscribers_;
};

} // namespace aistudio::core
