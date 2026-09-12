#pragma once

#include "Core/Plugin/PluginRegistry.hpp"
#include "Core/Security/PermissionPolicy.hpp"

namespace aistudio::core {

// Bridges PluginRegistry's declared data into PermissionPolicy -- the
// "Permission definition" checklist item (docs/ROADMAP.md Phase 11
// "Plugin SDK"), which PluginManifest::required_permissions has recorded
// since Phase 0 without anything ever consuming it. Lives under
// Core/Plugin/ rather than Core/Security/ so PermissionPolicy itself stays
// unaware Plugins exist (AGENT.md #2 responsibility separation) -- Plugin
// is the module that knows about both Plugin declarations and the
// Security policy they feed into, not the other way around.
//
// Adds every registered manifest's required_permissions pattern (see
// PluginRegistry::AllRequiredPermissions()) to `policy` via
// AddDangerousPattern() -- a Command whose name matches one of those
// patterns then requires approval under AutonomyLevel::Assisted the same
// way a platform default dangerous pattern does, regardless of which
// Backend actually dispatches it (PermissionPolicy checks Command::name,
// never which Backend produced it).
//
// Meant to be called once, after Plugin registration for a session is
// settled (mirroring the "PluginRegistry: all backend capabilities are
// declared by a manifest" bootstrap log line's own timing) -- calling it
// again after registering more manifests is harmless (duplicate patterns
// in PermissionPolicy's own list are redundant but not incorrect) but
// re-adds every previously-applied pattern again rather than diffing.
void ApplyRequiredPermissions(const PluginRegistry& registry, PermissionPolicy& policy);

} // namespace aistudio::core
