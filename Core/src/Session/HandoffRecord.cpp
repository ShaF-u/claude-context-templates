#include "Core/Session/HandoffRecord.hpp"

namespace aistudio::core {

std::string ToString(HandoffVerificationOutcome outcome) {
    switch (outcome) {
        case HandoffVerificationOutcome::Unknown: return "Unknown";
        case HandoffVerificationOutcome::Succeeded: return "Succeeded";
        case HandoffVerificationOutcome::Failed: return "Failed";
    }
    return "Unknown";
}

HandoffVerificationOutcome HandoffVerificationOutcomeFromString(const std::string& text) {
    if (text == "Succeeded") return HandoffVerificationOutcome::Succeeded;
    if (text == "Failed") return HandoffVerificationOutcome::Failed;
    return HandoffVerificationOutcome::Unknown; // "Unknown" itself, and any unrecognized value.
}

} // namespace aistudio::core
