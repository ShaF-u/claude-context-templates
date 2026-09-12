#pragma once

#include "Core/LLM/ILLMProvider.hpp"

namespace aistudio::core {

// A no-network ILLMProvider that echoes the last user message back,
// prefixed — proves the request/response/token-counting plumbing works
// end-to-end without a real API key, network call, or cost (the same
// role NullBackend plays for Backend System). Never a substitute for a
// real provider (Anthropic/OpenAI/... — docs/ROADMAP.md Phase 4), which
// need an explicit decision on API key handling before they call out to
// a real network endpoint.
class EchoLLMProvider final : public ILLMProvider {
public:
    [[nodiscard]] std::string Name() const override { return "echo"; }

    [[nodiscard]] Result<LLMResponse> Complete(const LLMRequest& request) override;
};

} // namespace aistudio::core
