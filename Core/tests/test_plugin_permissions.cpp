#include "test_framework.hpp"
#include "Core/Plugin/PluginPermissions.hpp"

using namespace aistudio::core;

namespace {

PluginManifest MakeManifest(std::string id, std::vector<std::string> required_permissions) {
    PluginManifest manifest;
    manifest.id = std::move(id);
    manifest.name = manifest.id;
    manifest.version = "0.0.1";
    manifest.required_permissions = std::move(required_permissions);
    return manifest;
}

Command MakeCommand(std::string name) {
    Command command;
    command.name = std::move(name);
    return command;
}

} // namespace

AISTUDIO_TEST(ApplyRequiredPermissions_NoManifests_LeavesPolicyUnchanged) {
    PluginRegistry registry;
    PermissionPolicy policy(AutonomyLevel::Assisted);

    ApplyRequiredPermissions(registry, policy);

    // Only the platform defaults apply -- an arbitrary plugin-shaped
    // command name that isn't one of PermissionPolicy::DefaultDangerousPatterns()
    // still doesn't require approval.
    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("plugin.custom.action")));
}

AISTUDIO_TEST(ApplyRequiredPermissions_ManifestPattern_RequiresApprovalForMatchingCommand) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("plugin.a", {"plugin.a.*"}));
    PermissionPolicy policy(AutonomyLevel::Assisted);

    ApplyRequiredPermissions(registry, policy);

    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("plugin.a.dangerous_action")));
}

AISTUDIO_TEST(ApplyRequiredPermissions_ManifestPattern_DoesNotAffectUnrelatedCommands) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("plugin.a", {"plugin.a.*"}));
    PermissionPolicy policy(AutonomyLevel::Assisted);

    ApplyRequiredPermissions(registry, policy);

    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("plugin.b.harmless_action")));
}

AISTUDIO_TEST(ApplyRequiredPermissions_MultipleManifests_AppliesAllPatterns) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("plugin.a", {"plugin.a.*"}));
    registry.RegisterManifest(MakeManifest("plugin.b", {"plugin.b.delete"}));
    PermissionPolicy policy(AutonomyLevel::Assisted);

    ApplyRequiredPermissions(registry, policy);

    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("plugin.a.anything")));
    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("plugin.b.delete")));
}

AISTUDIO_TEST(ApplyRequiredPermissions_AutonomousLevel_StillNeverRequiresApproval) {
    // PermissionPolicy::RequiresApproval() only consults dangerous_patterns_
    // under AutonomyLevel::Assisted -- Autonomous always returns false
    // regardless of what's been added, matching PermissionPolicy's own
    // existing precedent for its platform-default patterns.
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("plugin.a", {"plugin.a.*"}));
    PermissionPolicy policy(AutonomyLevel::Autonomous);

    ApplyRequiredPermissions(registry, policy);

    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("plugin.a.anything")));
}
