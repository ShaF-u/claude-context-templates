#pragma once

#include "Core/Protocol/Command.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// How much the Agent may act without a human confirming first —
// docs/MASTER_SPEC.md #41 Human Approval Policy.
enum class AutonomyLevel {
    Manual,               // everything requires approval
    Assisted,              // only dangerous operations require approval
    Autonomous,            // nothing requires approval
    RestrictedAutonomous,  // autonomous only for whitelisted backend ids
};

[[nodiscard]] std::string ToString(AutonomyLevel level);

// Decides whether a Command needs human approval before it actually runs
// — AGENT.md #11: file delete, git push, production deploy, and other
// destructive/high-blast-radius actions need confirmation by default.
// Deliberately name/pattern-based rather than inspecting Command::payload
// — a Backend's Capability names (docs/MASTER_SPEC.md #32) are the
// vocabulary this checks against, so a new dangerous Capability is
// covered just by matching its name, no per-Backend code required.
class PermissionPolicy {
public:
    explicit PermissionPolicy(AutonomyLevel level = AutonomyLevel::Assisted);

    void SetAutonomyLevel(AutonomyLevel level) { level_ = level; }
    [[nodiscard]] AutonomyLevel Level() const { return level_; }

    // Glob-lite pattern (Core/Util/Glob) checked against Command::name,
    // e.g. "*.delete", "git.push", "*.deploy".
    void AddDangerousPattern(std::string pattern);
    [[nodiscard]] static std::vector<std::string> DefaultDangerousPatterns();

    // For RestrictedAutonomous: backend ids that may act without approval.
    void AllowAutonomousBackend(std::string backend_id);

    [[nodiscard]] bool RequiresApproval(const Command& command) const;

private:
    AutonomyLevel level_;
    std::vector<std::string> dangerous_patterns_;
    std::vector<std::string> autonomous_backend_ids_;
};

} // namespace aistudio::core
