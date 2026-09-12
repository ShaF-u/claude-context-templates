#include "Core/Plugin/PluginRegistry.hpp"

#include "Core/Event/EventBus.hpp"

#include <algorithm>

namespace aistudio::core {

std::string ToString(PluginLifecycleState state) {
    switch (state) {
        case PluginLifecycleState::Registered: return "Registered";
        case PluginLifecycleState::Enabled: return "Enabled";
        case PluginLifecycleState::Disabled: return "Disabled";
    }
    return "Unknown";
}

Result<void> PluginRegistry::RegisterManifest(PluginManifest manifest) {
    if (manifest.id.empty()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "cannot register a manifest with an empty id",
            .module = "Core.Plugin.Registry",
        });
    }
    if (manifests_.contains(manifest.id)) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "manifest already registered: " + manifest.id,
            .module = "Core.Plugin.Registry",
        });
    }
    const auto id = manifest.id;
    manifests_.emplace(id, std::move(manifest));
    lifecycle_states_[id] = PluginLifecycleState::Registered;
    return Result<void>::Ok();
}

void PluginRegistry::UnregisterManifest(const std::string& id) {
    manifests_.erase(id);
    lifecycle_states_.erase(id);
}

std::optional<PluginManifest> PluginRegistry::Find(const std::string& id) const {
    const auto it = manifests_.find(id);
    if (it == manifests_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<PluginManifest> PluginRegistry::FindCompatible(const std::string& id,
                                                               std::optional<CapabilityVersion> required_version) const {
    const auto it = manifests_.find(id);
    if (it == manifests_.end()) {
        return std::nullopt;
    }
    const auto provided_version = CapabilityVersion::Parse(it->second.version);
    if (!IsCompatible(required_version, provided_version)) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<PluginManifest> PluginRegistry::All() const {
    std::vector<PluginManifest> result;
    result.reserve(manifests_.size());
    for (const auto& [id, manifest] : manifests_) {
        result.push_back(manifest);
    }
    return result;
}

std::vector<std::string> PluginRegistry::UndeclaredCapabilities(const BackendRegistry& registry) const {
    std::vector<std::string> declared;
    for (const auto& [id, manifest] : manifests_) {
        declared.insert(declared.end(), manifest.capabilities.begin(), manifest.capabilities.end());
    }

    std::vector<std::string> undeclared;
    for (const auto& backend : registry.All()) {
        for (const auto& capability : backend->Capabilities()) {
            if (std::find(declared.begin(), declared.end(), capability) == declared.end() &&
                std::find(undeclared.begin(), undeclared.end(), capability) == undeclared.end()) {
                undeclared.push_back(capability);
            }
        }
    }
    return undeclared;
}

std::vector<std::string> PluginRegistry::AllRequiredPermissions() const {
    std::vector<std::string> patterns;
    for (const auto& [id, manifest] : manifests_) {
        for (const auto& pattern : manifest.required_permissions) {
            if (std::find(patterns.begin(), patterns.end(), pattern) == patterns.end()) {
                patterns.push_back(pattern);
            }
        }
    }
    return patterns;
}

void PluginRegistry::TransitionTo(const std::string& id, PluginLifecycleState new_state) {
    const auto previous = lifecycle_states_.at(id);
    if (previous == new_state) {
        return; // idempotent no-op, no event for a state that didn't change
    }
    lifecycle_states_[id] = new_state;
    EventBus::Instance().Publish("PluginLifecycleChanged", PluginLifecycleChange{id, previous, new_state});
}

Result<void> PluginRegistry::EnablePlugin(const std::string& id) {
    if (!manifests_.contains(id)) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "no plugin manifest registered with id: " + id,
            .module = "Core.Plugin.Registry",
        });
    }
    TransitionTo(id, PluginLifecycleState::Enabled);
    return Result<void>::Ok();
}

Result<void> PluginRegistry::DisablePlugin(const std::string& id) {
    if (!manifests_.contains(id)) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "no plugin manifest registered with id: " + id,
            .module = "Core.Plugin.Registry",
        });
    }
    TransitionTo(id, PluginLifecycleState::Disabled);
    return Result<void>::Ok();
}

std::optional<PluginLifecycleState> PluginRegistry::LifecycleState(const std::string& id) const {
    const auto it = lifecycle_states_.find(id);
    return it == lifecycle_states_.end() ? std::nullopt : std::make_optional(it->second);
}

} // namespace aistudio::core
