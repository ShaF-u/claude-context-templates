#pragma once

#include <string>
#include <vector>

namespace aistudio::core {

// Declares what a Plugin provides, without Core needing to know its
// internals — docs/MASTER_SPEC.md #70 Plugin System. Phase 0 P1's scope
// is this manifest shape and a registry to track it (design-time
// bookkeeping); a full Plugin SDK — dynamic/out-of-process loading,
// lifecycle hooks, Context Provider and UI Extension wiring — is
// docs/ROADMAP.md Phase 11 and stays deferred.
struct PluginManifest {
    std::string id;
    std::string name;
    std::string version;

    // Capability names (matching IBackend::Capabilities()) this plugin's
    // Backend(s) provide.
    std::vector<std::string> capabilities;

    // Event names this plugin may publish via EventBus.
    std::vector<std::string> provided_events;

    // Command-name patterns (PermissionPolicy glob syntax) this plugin's
    // Commands need approval-gated access to, beyond the platform
    // defaults — e.g. a plugin that itself performs deletions.
    std::vector<std::string> required_permissions;
};

// A registered Plugin's Enable/Disable state (docs/ROADMAP.md Phase 11
// "Plugin lifecycle") — the same design-time-bookkeeping level as
// PluginManifest/PluginRegistry itself, not a dynamic-loading state
// machine: there's no code to actually start/stop yet, only a
// declaration a user or admin can toggle on/off. Mirrors
// BackendLifecycleState's shape (see IBackend.hpp) without carrying over
// states that only make sense once there's a Start()/Stop() to call
// (Starting/Stopping/Failed). A freshly registered manifest starts at
// Registered, having never been explicitly enabled.
enum class PluginLifecycleState { Registered, Enabled, Disabled };

[[nodiscard]] std::string ToString(PluginLifecycleState state);

} // namespace aistudio::core
