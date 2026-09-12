#include "Core/Backend/BackendRegistry.hpp"

#include "Core/Event/EventBus.hpp"

namespace aistudio::core {

Result<void> BackendRegistry::Register(std::shared_ptr<IBackend> backend) {
    if (!backend) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "cannot register a null backend",
            .module = "Core.Backend.Registry",
        });
    }
    const auto id = backend->Id();
    if (backends_.contains(id)) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "backend already registered: " + id,
            .module = "Core.Backend.Registry",
        });
    }
    backends_.emplace(id, std::move(backend));
    lifecycle_states_[id] = BackendLifecycleState::Registered;
    return Result<void>::Ok();
}

void BackendRegistry::Unregister(const std::string& id) {
    backends_.erase(id);
    lifecycle_states_.erase(id);
}

std::shared_ptr<IBackend> BackendRegistry::Find(const std::string& id) const {
    const auto it = backends_.find(id);
    return it == backends_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<IBackend>> BackendRegistry::FindByCapability(
    const std::string& capability_name, std::optional<CapabilityVersion> required_version) const {
    std::vector<std::shared_ptr<IBackend>> result;
    for (const auto& [id, backend] : backends_) {
        for (const auto& raw : backend->Capabilities()) {
            const auto capability = Capability::Parse(raw);
            if (capability.name == capability_name && IsCompatible(required_version, capability.version)) {
                result.push_back(backend);
                break;
            }
        }
    }
    return result;
}

std::vector<std::shared_ptr<IBackend>> BackendRegistry::All() const {
    std::vector<std::shared_ptr<IBackend>> result;
    result.reserve(backends_.size());
    for (const auto& [id, backend] : backends_) {
        result.push_back(backend);
    }
    return result;
}

CommandResult BackendRegistry::Dispatch(Command command) const {
    const auto backend = Find(command.backend_id);
    if (!backend) {
        return CommandResult::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "no backend registered with id: " + command.backend_id,
            .module = "Core.Backend.Registry",
        });
    }
    if (command.request_id.empty()) {
        command.request_id = RequestIdGenerator::Next();
    }
    return backend->Dispatch(command);
}

QueryResult BackendRegistry::RunQuery(Query query) const {
    const auto backend = Find(query.backend_id);
    if (!backend) {
        return QueryResult::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "no backend registered with id: " + query.backend_id,
            .module = "Core.Backend.Registry",
        });
    }
    if (query.request_id.empty()) {
        query.request_id = RequestIdGenerator::Next();
    }
    return backend->Handle(query);
}

Result<void> BackendRegistry::ConfigureAll(const Config& config) const {
    std::string combined_message;
    for (const auto& [id, backend] : backends_) {
        if (const auto result = backend->Configure(config); !result) {
            if (!combined_message.empty()) {
                combined_message += "; ";
            }
            combined_message += id + ": " + result.Err().message;
        }
    }
    if (!combined_message.empty()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "one or more backends failed to configure: " + combined_message,
            .module = "Core.Backend.Registry",
        });
    }
    return Result<void>::Ok();
}

std::vector<std::pair<std::string, BackendHealth>> BackendRegistry::CheckHealth() {
    std::vector<std::pair<std::string, BackendHealth>> readings;
    readings.reserve(backends_.size());

    for (const auto& [id, backend] : backends_) {
        const auto current = backend->Health();
        readings.emplace_back(id, current);

        const auto it = last_health_.find(id);
        const bool changed = it == last_health_.end() || it->second != current;
        if (changed) {
            const auto previous = it == last_health_.end() ? BackendHealth::Unknown : it->second;
            EventBus::Instance().Publish("BackendHealthChanged", BackendHealthChange{id, previous, current});
            last_health_[id] = current;
        }
    }

    return readings;
}

void BackendRegistry::TransitionTo(const std::string& id, BackendLifecycleState new_state) {
    const auto previous = lifecycle_states_.at(id);
    lifecycle_states_[id] = new_state;
    EventBus::Instance().Publish("BackendLifecycleChanged", BackendLifecycleChange{id, previous, new_state});
}

Result<void> BackendRegistry::StartBackend(const std::string& id) {
    const auto backend_it = backends_.find(id);
    if (backend_it == backends_.end()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "no backend registered with id: " + id,
            .module = "Core.Backend.Registry",
        });
    }
    const auto current = lifecycle_states_.at(id);
    if (current == BackendLifecycleState::Starting || current == BackendLifecycleState::Running ||
        current == BackendLifecycleState::Stopping) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "backend '" + id + "' cannot start from state " + ToString(current),
            .module = "Core.Backend.Registry",
        });
    }

    TransitionTo(id, BackendLifecycleState::Starting);
    const auto start_result = backend_it->second->Start();
    if (!start_result) {
        TransitionTo(id, BackendLifecycleState::Failed);
        return start_result;
    }
    TransitionTo(id, BackendLifecycleState::Running);
    return Result<void>::Ok();
}

Result<void> BackendRegistry::StopBackend(const std::string& id) {
    const auto backend_it = backends_.find(id);
    if (backend_it == backends_.end()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "no backend registered with id: " + id,
            .module = "Core.Backend.Registry",
        });
    }
    const auto current = lifecycle_states_.at(id);
    if (current != BackendLifecycleState::Running) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "backend '" + id + "' cannot stop from state " + ToString(current),
            .module = "Core.Backend.Registry",
        });
    }

    TransitionTo(id, BackendLifecycleState::Stopping);
    const auto stop_result = backend_it->second->Stop();
    if (!stop_result) {
        TransitionTo(id, BackendLifecycleState::Failed);
        return stop_result;
    }
    TransitionTo(id, BackendLifecycleState::Stopped);
    return Result<void>::Ok();
}

Result<void> BackendRegistry::StartAll() {
    std::string combined_message;
    for (const auto& [id, backend] : backends_) {
        if (const auto result = StartBackend(id); !result) {
            if (!combined_message.empty()) {
                combined_message += "; ";
            }
            combined_message += id + ": " + result.Err().message;
        }
    }
    if (!combined_message.empty()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "one or more backends failed to start: " + combined_message,
            .module = "Core.Backend.Registry",
        });
    }
    return Result<void>::Ok();
}

Result<void> BackendRegistry::StopAll() {
    std::string combined_message;
    for (const auto& [id, backend] : backends_) {
        if (const auto result = StopBackend(id); !result) {
            if (!combined_message.empty()) {
                combined_message += "; ";
            }
            combined_message += id + ": " + result.Err().message;
        }
    }
    if (!combined_message.empty()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "one or more backends failed to stop: " + combined_message,
            .module = "Core.Backend.Registry",
        });
    }
    return Result<void>::Ok();
}

std::optional<BackendLifecycleState> BackendRegistry::LifecycleState(const std::string& id) const {
    const auto it = lifecycle_states_.find(id);
    return it == lifecycle_states_.end() ? std::nullopt : std::make_optional(it->second);
}

} // namespace aistudio::core
