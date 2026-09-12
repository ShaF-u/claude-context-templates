#include "Core/Event/EventBus.hpp"

#include <algorithm>

namespace aistudio::core {

EventBus& EventBus::Instance() {
    static EventBus instance;
    return instance;
}

int EventBus::Subscribe(const std::string& event_name, EventHandler handler) {
    std::lock_guard lock(mutex_);
    const int id = next_id_++;
    subscribers_[event_name].push_back(Subscription{id, std::move(handler), nullptr});
    return id;
}

int EventBus::SubscribeEvent(const std::string& event_name, FullEventHandler handler) {
    std::lock_guard lock(mutex_);
    const int id = next_id_++;
    subscribers_[event_name].push_back(Subscription{id, nullptr, std::move(handler)});
    return id;
}

void EventBus::Unsubscribe(const std::string& event_name, int subscription_id) {
    std::lock_guard lock(mutex_);
    auto it = subscribers_.find(event_name);
    if (it == subscribers_.end()) return;
    auto& list = it->second;
    list.erase(std::remove_if(list.begin(), list.end(),
                               [subscription_id](const Subscription& s) { return s.id == subscription_id; }),
               list.end());
}

void EventBus::Publish(const Event& event) {
    std::vector<Subscription> handlers_copy;
    {
        std::lock_guard lock(mutex_);
        auto it = subscribers_.find(event.name);
        if (it == subscribers_.end()) return;
        handlers_copy = it->second;
    }
    for (const auto& sub : handlers_copy) {
        if (sub.full_handler) {
            sub.full_handler(event);
        } else if (sub.handler) {
            sub.handler(event.payload);
        }
    }
}

void EventBus::Publish(const std::string& event_name, const std::any& payload) {
    Publish(Event{.name = event_name, .payload = payload});
}

} // namespace aistudio::core
