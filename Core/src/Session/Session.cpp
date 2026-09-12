#include "Core/Session/Session.hpp"

namespace aistudio::core {

std::string ToString(SessionState state) {
    switch (state) {
        case SessionState::Unknown: return "Unknown";
        case SessionState::Starting: return "Starting";
        case SessionState::Running: return "Running";
        case SessionState::WaitingForInput: return "WaitingForInput";
        case SessionState::WaitingForApproval: return "WaitingForApproval";
        case SessionState::Terminated: return "Terminated";
        case SessionState::Failed: return "Failed";
    }
    return "Unknown";
}

} // namespace aistudio::core
