#pragma once

#include "Core/Error/Result.hpp"
#include "Core/LLM/LLMRequest.hpp"
#include "Core/LLM/LLMResponse.hpp"

#include <functional>
#include <string>

namespace aistudio::core {

// Common surface every LLM provider (Anthropic, OpenAI, Google, Ollama,
// llama.cpp, LM Studio, Custom — docs/ROADMAP.md Phase 4 "Provider")
// implements, so swapping providers doesn't touch call sites — the same
// role IBackend plays for Backends (AGENT.md #2). This PR's only
// implementation is EchoLLMProvider, a no-network stand-in that proves
// the request/response/streaming plumbing works end-to-end before a
// real provider exists, the same bootstrapping order Backend System used
// (NullBackend before Unreal/Unity/Git).
class ILLMProvider {
public:
    virtual ~ILLMProvider() = default;

    [[nodiscard]] virtual std::string Name() const = 0;

    [[nodiscard]] virtual Result<LLMResponse> Complete(const LLMRequest& request) = 0;

    // Default: delivers the whole response as a single chunk once
    // Complete() finishes — a baseline every provider gets for free just
    // by implementing Complete(). A provider with a real incremental API
    // (e.g. Anthropic's SSE streaming) overrides this for true
    // token-by-token delivery (docs/ROADMAP.md Phase 4 "Streaming").
    virtual Result<LLMResponse> CompleteStreaming(const LLMRequest& request,
                                                   const std::function<void(const std::string&)>& on_chunk) {
        auto result = Complete(request);
        if (result) {
            on_chunk(result.Value().content);
        }
        return result;
    }
};

} // namespace aistudio::core
