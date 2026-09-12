#pragma once

#include <cstdint>
#include <string>

namespace aistudio::core {

// Token counting (docs/ROADMAP.md Phase 4 "LLM Features" > "Token
// counting") for one completion — separate input/output counts since
// providers price and report them separately.
struct LLMUsage {
    std::int64_t input_tokens = 0;
    std::int64_t output_tokens = 0;
};

// A provider-agnostic chat-completion result. `stop_reason` is whatever
// string the provider returns (e.g. "end_turn", "max_tokens") — not an
// enum, since the set of reasons isn't standardized across providers and
// enumerating them speculatively isn't worth it yet (AGENT.md #14).
struct LLMResponse {
    std::string content;
    std::string stop_reason;
    LLMUsage usage;
};

} // namespace aistudio::core
