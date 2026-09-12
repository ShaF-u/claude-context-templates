#include "Core/Security/PermissionPolicy.hpp"

#include "Core/Util/Glob.hpp"

#include <algorithm>

namespace aistudio::core {

namespace {
bool MatchesAny(const std::string& text, const std::vector<std::string>& patterns) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& pattern) {
        return GlobMatch(text, pattern);
    });
}
} // namespace

std::string ToString(AutonomyLevel level) {
    switch (level) {
        case AutonomyLevel::Manual: return "Manual";
        case AutonomyLevel::Assisted: return "Assisted";
        case AutonomyLevel::Autonomous: return "Autonomous";
        case AutonomyLevel::RestrictedAutonomous: return "RestrictedAutonomous";
    }
    return "Unknown";
}

PermissionPolicy::PermissionPolicy(AutonomyLevel level) : level_(level), dangerous_patterns_(DefaultDangerousPatterns()) {}

std::vector<std::string> PermissionPolicy::DefaultDangerousPatterns() {
    return {
        "*.delete", "*.remove", "git.push", "*.deploy", "settings.write", "security.*",
    };
}

void PermissionPolicy::AddDangerousPattern(std::string pattern) {
    dangerous_patterns_.push_back(std::move(pattern));
}

void PermissionPolicy::AllowAutonomousBackend(std::string backend_id) {
    autonomous_backend_ids_.push_back(std::move(backend_id));
}

bool PermissionPolicy::RequiresApproval(const Command& command) const {
    switch (level_) {
        case AutonomyLevel::Manual:
            return true;
        case AutonomyLevel::Autonomous:
            return false;
        case AutonomyLevel::Assisted:
            return MatchesAny(command.name, dangerous_patterns_);
        case AutonomyLevel::RestrictedAutonomous:
            return std::find(autonomous_backend_ids_.begin(), autonomous_backend_ids_.end(), command.backend_id) ==
                   autonomous_backend_ids_.end();
    }
    return true;
}

} // namespace aistudio::core
