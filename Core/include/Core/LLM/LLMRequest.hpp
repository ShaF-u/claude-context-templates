#pragma once

#include "Core/LLM/ChatMessage.hpp"

#include <optional>
#include <string>
#include <vector>

namespace aistudio::core {

// A provider-agnostic chat-completion request — docs/ROADMAP.md Phase 4
// "LLM Integration". `model` is whatever string the target ILLMProvider
// expects (e.g. an Anthropic model id) — this type doesn't validate or
// interpret it, matching Command::name's own "the Backend's vocabulary,
// not ours" stance.
struct LLMRequest {
    std::string model;
    std::vector<ChatMessage> messages;
    int max_tokens = 1024;
    std::optional<double> temperature;
};

} // namespace aistudio::core
