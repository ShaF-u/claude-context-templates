#include "Core/Session/ResourceLeaseLedger.hpp"

#include "Core/Event/EventBus.hpp"

#include <algorithm>

namespace aistudio::core {

namespace {

bool HasEntryFor(const std::vector<ResourceLeaseHolder>& holders, const std::string& session_id) {
    return std::any_of(holders.begin(), holders.end(),
                        [&](const ResourceLeaseHolder& holder) { return holder.session_id == session_id; });
}

bool AnyExclusive(const std::vector<ResourceLeaseHolder>& holders) {
    return std::any_of(holders.begin(), holders.end(),
                        [](const ResourceLeaseHolder& holder) { return holder.mode == LeaseMode::Exclusive; });
}

} // namespace

Result<LeaseAcquireResult> ResourceLeaseLedger::Acquire(const std::string& resource_name, const std::string& session_id,
                                                          LeaseMode mode) {
    std::lock_guard lock(mutex_);
    ResourceState& state = resources_[resource_name];

    if (HasEntryFor(state.holders, session_id) || HasEntryFor(state.wait_queue, session_id)) {
        return Result<LeaseAcquireResult>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                                        .message = "session '" + session_id +
                                                                    "' already holds or is queued for resource '" +
                                                                    resource_name + "'",
                                                        .module = "Core.Session.ResourceLeaseLedger"});
    }

    // A new Shared request only jumps straight in when NOTHING is
    // already waiting -- otherwise it would starve whoever's ahead in
    // the FIFO (see class comment).
    const bool compatible_now =
        state.wait_queue.empty() && (mode == LeaseMode::Shared ? !AnyExclusive(state.holders) : state.holders.empty());
    if (compatible_now) {
        state.holders.push_back(ResourceLeaseHolder{session_id, mode});
        return Result<LeaseAcquireResult>::Ok(LeaseAcquireResult{LeaseAcquireOutcome::Acquired, {}});
    }

    state.wait_queue.push_back(ResourceLeaseHolder{session_id, mode});
    return Result<LeaseAcquireResult>::Ok(LeaseAcquireResult{LeaseAcquireOutcome::Queued, state.holders});
}

void ResourceLeaseLedger::PromoteQueued(ResourceState& state, std::vector<ResourceLeaseHolder>& promoted) {
    while (!state.wait_queue.empty()) {
        const ResourceLeaseHolder& front = state.wait_queue.front();
        const bool can_promote =
            front.mode == LeaseMode::Shared ? !AnyExclusive(state.holders) : state.holders.empty();
        if (!can_promote) {
            break; // FIFO: never skip ahead of a request that isn't promotable yet
        }
        state.holders.push_back(front);
        promoted.push_back(front);
        const bool promoted_was_exclusive = front.mode == LeaseMode::Exclusive;
        state.wait_queue.erase(state.wait_queue.begin());
        if (promoted_was_exclusive) {
            break; // an Exclusive holder now occupies the resource alone
        }
    }
}

Result<void> ResourceLeaseLedger::Release(const std::string& resource_name, const std::string& session_id) {
    std::vector<ResourceLeaseHolder> promoted;
    {
        std::lock_guard lock(mutex_);
        const auto it = resources_.find(resource_name);
        if (it == resources_.end()) {
            return Result<void>::Fail(Error{.code = ErrorCode::NotFound,
                                             .message = "session '" + session_id + "' is not tracked for resource '" +
                                                         resource_name + "'",
                                             .module = "Core.Session.ResourceLeaseLedger"});
        }
        ResourceState& state = it->second;

        const auto holder_it = std::find_if(state.holders.begin(), state.holders.end(),
                                             [&](const ResourceLeaseHolder& h) { return h.session_id == session_id; });
        const auto queued_it = std::find_if(state.wait_queue.begin(), state.wait_queue.end(),
                                             [&](const ResourceLeaseHolder& h) { return h.session_id == session_id; });
        if (holder_it != state.holders.end()) {
            state.holders.erase(holder_it);
        } else if (queued_it != state.wait_queue.end()) {
            state.wait_queue.erase(queued_it);
        } else {
            return Result<void>::Fail(Error{.code = ErrorCode::NotFound,
                                             .message = "session '" + session_id +
                                                         "' neither holds nor is queued for resource '" + resource_name + "'",
                                             .module = "Core.Session.ResourceLeaseLedger"});
        }

        PromoteQueued(state, promoted);
    }
    for (const auto& holder : promoted) {
        EventBus::Instance().Publish("ResourceLeaseAcquired", ResourceLeaseAcquiredEvent{resource_name, holder});
    }
    return Result<void>::Ok();
}

void ResourceLeaseLedger::ReleaseAllForSession(const std::string& session_id) {
    std::vector<ResourceLeaseAcquiredEvent> promoted_events;
    {
        std::lock_guard lock(mutex_);
        for (auto& [resource_name, state] : resources_) {
            const auto holder_it = std::find_if(state.holders.begin(), state.holders.end(),
                                                 [&](const ResourceLeaseHolder& h) { return h.session_id == session_id; });
            const auto queued_it = std::find_if(state.wait_queue.begin(), state.wait_queue.end(),
                                                 [&](const ResourceLeaseHolder& h) { return h.session_id == session_id; });
            bool removed_something = false;
            if (holder_it != state.holders.end()) {
                state.holders.erase(holder_it);
                removed_something = true;
            }
            if (queued_it != state.wait_queue.end()) {
                state.wait_queue.erase(queued_it);
                removed_something = true;
            }
            if (!removed_something) {
                continue;
            }
            std::vector<ResourceLeaseHolder> promoted;
            PromoteQueued(state, promoted);
            for (const auto& holder : promoted) {
                promoted_events.push_back(ResourceLeaseAcquiredEvent{resource_name, holder});
            }
        }
    }
    for (const auto& event : promoted_events) {
        EventBus::Instance().Publish("ResourceLeaseAcquired", event);
    }
}

std::vector<ResourceLeaseHolder> ResourceLeaseLedger::HoldersOf(const std::string& resource_name) const {
    std::lock_guard lock(mutex_);
    const auto it = resources_.find(resource_name);
    return it == resources_.end() ? std::vector<ResourceLeaseHolder>{} : it->second.holders;
}

std::vector<std::string> ResourceLeaseLedger::WaitQueueFor(const std::string& resource_name) const {
    std::lock_guard lock(mutex_);
    const auto it = resources_.find(resource_name);
    if (it == resources_.end()) {
        return {};
    }
    std::vector<std::string> ids;
    ids.reserve(it->second.wait_queue.size());
    for (const auto& holder : it->second.wait_queue) {
        ids.push_back(holder.session_id);
    }
    return ids;
}

} // namespace aistudio::core
