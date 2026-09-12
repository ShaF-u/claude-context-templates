#pragma once

#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Backend/CapabilityVersion.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Plugin/PluginManifest.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// Published as a "PluginLifecycleChanged" event for every EnablePlugin()/
// DisablePlugin() call that actually changes state — mirrors
// BackendLifecycleChange (see BackendRegistry.hpp) in shape and in when
// it's published.
struct PluginLifecycleChange {
    std::string plugin_id;
    PluginLifecycleState previous;
    PluginLifecycleState current;
};

// Tracks declared PluginManifests — the design-time counterpart to
// BackendFactoryRegistry's runtime self-registration
// (docs/ROADMAP.md Phase 0 P1 "Plugin protocol design").
class PluginRegistry {
public:
    Result<void> RegisterManifest(PluginManifest manifest);
    void UnregisterManifest(const std::string& id);

    [[nodiscard]] std::optional<PluginManifest> Find(const std::string& id) const;
    [[nodiscard]] std::vector<PluginManifest> All() const;

    // Like Find(), but additionally requires the manifest's own `version`
    // (parsed the same way a Capability's "@major.minor.patch" suffix is —
    // see CapabilityVersion::Parse) to satisfy `required_version` under
    // the same negotiation rule BackendRegistry::FindByCapability uses for
    // Capabilities (docs/ROADMAP.md Phase 11 "Versioning"): same major
    // version, and the manifest's version >= required within it. nullopt
    // `required_version` matches any manifest, including one whose
    // `version` string doesn't parse. Returns nullopt if `id` isn't
    // registered OR its version doesn't satisfy `required_version`.
    [[nodiscard]] std::optional<PluginManifest> FindCompatible(
        const std::string& id, std::optional<CapabilityVersion> required_version) const;

    // Capabilities a registered Backend advertises (IBackend::Capabilities())
    // that no registered manifest declares — signals a plugin whose
    // manifest is stale or missing (AGENT.md #9: don't let drift between
    // what's declared and what's actually implemented go unnoticed).
    [[nodiscard]] std::vector<std::string> UndeclaredCapabilities(const BackendRegistry& registry) const;

    // The deduplicated union of every registered manifest's
    // PluginManifest::required_permissions — see PluginPermissions.hpp's
    // ApplyRequiredPermissions() for what consumes this. Includes every
    // manifest regardless of PluginLifecycleState (same as
    // UndeclaredCapabilities() above, which also doesn't filter by
    // lifecycle) -- a Disabled plugin's declared pattern still requiring
    // approval is the conservative direction, and nothing currently gates
    // actual Command dispatch on lifecycle state anyway.
    [[nodiscard]] std::vector<std::string> AllRequiredPermissions() const;

    // Registered/Disabled -> Enabled, and Registered/Enabled -> Disabled
    // (docs/ROADMAP.md Phase 11 "Plugin lifecycle"). Both are idempotent
    // no-ops (Ok, no event published) when already in the target state;
    // both fail with NotFound for an unregistered id.
    Result<void> EnablePlugin(const std::string& id);
    Result<void> DisablePlugin(const std::string& id);

    // Registered for a manifest that's never had EnablePlugin()/
    // DisablePlugin() called on it; nullopt if `id` was never registered
    // (distinct from Registered, same as BackendRegistry::LifecycleState).
    [[nodiscard]] std::optional<PluginLifecycleState> LifecycleState(const std::string& id) const;

private:
    void TransitionTo(const std::string& id, PluginLifecycleState new_state);

    std::unordered_map<std::string, PluginManifest> manifests_;
    std::unordered_map<std::string, PluginLifecycleState> lifecycle_states_;
};

} // namespace aistudio::core
