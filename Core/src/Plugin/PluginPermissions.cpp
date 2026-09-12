#include "Core/Plugin/PluginPermissions.hpp"

namespace aistudio::core {

void ApplyRequiredPermissions(const PluginRegistry& registry, PermissionPolicy& policy) {
    auto patterns = registry.AllRequiredPermissions();
    for (auto& pattern : patterns) {
        policy.AddDangerousPattern(std::move(pattern));
    }
}

} // namespace aistudio::core
