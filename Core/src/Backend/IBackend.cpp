#include "Core/Backend/IBackend.hpp"

namespace aistudio::core {

std::string ToString(BackendHealth health) {
    switch (health) {
        case BackendHealth::Unknown: return "Unknown";
        case BackendHealth::Healthy: return "Healthy";
        case BackendHealth::Degraded: return "Degraded";
        case BackendHealth::Unavailable: return "Unavailable";
    }
    return "Unknown";
}

std::string ToString(BackendLifecycleState state) {
    switch (state) {
        case BackendLifecycleState::Registered: return "Registered";
        case BackendLifecycleState::Starting: return "Starting";
        case BackendLifecycleState::Running: return "Running";
        case BackendLifecycleState::Stopping: return "Stopping";
        case BackendLifecycleState::Stopped: return "Stopped";
        case BackendLifecycleState::Failed: return "Failed";
    }
    return "Unknown";
}

} // namespace aistudio::core
