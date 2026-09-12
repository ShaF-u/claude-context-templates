#pragma once

#include <any>
#include <string>

namespace aistudio::core {

// A notification that something happened (a state change), published via
// EventBus by its `name`. Distinct from EventBus itself: this is the
// message shape, EventBus is the delivery mechanism (AGENT.md #5).
// `correlation_id` ties the Event back to the Command/Query that caused
// it, when there is one, so a Command's downstream effects stay traceable.
struct Event {
    std::string request_id;
    std::string correlation_id;
    std::string source; // Backend id or Core module name that emitted this
    std::string name;   // e.g. "BackendConnected", "TaskCompleted"
    std::any payload;
};

} // namespace aistudio::core
