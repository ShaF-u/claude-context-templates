#include "Core/LLM/ChatMessage.hpp"

namespace aistudio::core {

std::string ToString(ChatRole role) {
    switch (role) {
        case ChatRole::System: return "System";
        case ChatRole::User: return "User";
        case ChatRole::Assistant: return "Assistant";
    }
    return "Unknown";
}

} // namespace aistudio::core
