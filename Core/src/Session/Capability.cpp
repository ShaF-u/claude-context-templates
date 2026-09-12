#include "Core/Session/Capability.hpp"

namespace aistudio::core {

std::string ToString(SupportLevel level) {
    switch (level) {
        case SupportLevel::Unknown: return "Unknown";
        case SupportLevel::Supported: return "Supported";
        case SupportLevel::Unsupported: return "Unsupported";
    }
    return "Unknown";
}

} // namespace aistudio::core
