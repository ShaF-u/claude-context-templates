#include "test_framework.hpp"
#include "Core/Security/PermissionPolicy.hpp"

using namespace aistudio::core;

namespace {
Command MakeCommand(std::string backend_id, std::string name) {
    Command command;
    command.backend_id = std::move(backend_id);
    command.name = std::move(name);
    return command;
}
} // namespace

AISTUDIO_TEST(PermissionPolicy_Manual_RequiresApprovalForEverything) {
    PermissionPolicy policy(AutonomyLevel::Manual);
    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("core.null", "diagnostics.ping")));
}

AISTUDIO_TEST(PermissionPolicy_Autonomous_NeverRequiresApproval) {
    PermissionPolicy policy(AutonomyLevel::Autonomous);
    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("git", "git.push")));
}

AISTUDIO_TEST(PermissionPolicy_Assisted_OnlyDangerousPatternsRequireApproval) {
    PermissionPolicy policy(AutonomyLevel::Assisted);
    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("core.null", "diagnostics.ping")));
    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("git", "git.push")));
    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("assets", "asset.delete")));
}

AISTUDIO_TEST(PermissionPolicy_AddDangerousPattern_ExtendsDefaults) {
    PermissionPolicy policy(AutonomyLevel::Assisted);
    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("custom", "custom.risky")));

    policy.AddDangerousPattern("custom.risky");
    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("custom", "custom.risky")));
}

AISTUDIO_TEST(PermissionPolicy_RestrictedAutonomous_AllowsOnlyWhitelistedBackend) {
    PermissionPolicy policy(AutonomyLevel::RestrictedAutonomous);
    policy.AllowAutonomousBackend("trusted");

    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("trusted", "anything")));
    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("untrusted", "anything")));
}

AISTUDIO_TEST(PermissionPolicy_DefaultConstructed_IsAssisted) {
    const PermissionPolicy policy;
    AISTUDIO_EXPECT(policy.Level() == AutonomyLevel::Assisted);
}

AISTUDIO_TEST(PermissionPolicy_SetAutonomyLevel_ChangesBehavior) {
    PermissionPolicy policy(AutonomyLevel::Manual);
    AISTUDIO_EXPECT(policy.RequiresApproval(MakeCommand("core.null", "diagnostics.ping")));

    policy.SetAutonomyLevel(AutonomyLevel::Autonomous);
    AISTUDIO_EXPECT(!policy.RequiresApproval(MakeCommand("core.null", "diagnostics.ping")));
}

AISTUDIO_TEST(ToString_AutonomyLevel_CoversAllValues) {
    AISTUDIO_EXPECT(ToString(AutonomyLevel::Manual) == "Manual");
    AISTUDIO_EXPECT(ToString(AutonomyLevel::Assisted) == "Assisted");
    AISTUDIO_EXPECT(ToString(AutonomyLevel::Autonomous) == "Autonomous");
    AISTUDIO_EXPECT(ToString(AutonomyLevel::RestrictedAutonomous) == "RestrictedAutonomous");
}
