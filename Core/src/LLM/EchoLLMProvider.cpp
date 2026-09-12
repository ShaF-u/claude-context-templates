#include "Core/LLM/EchoLLMProvider.hpp"

#include "Core/Context/ContextItem.hpp"

namespace aistudio::core {

Result<LLMResponse> EchoLLMProvider::Complete(const LLMRequest& request) {
    if (request.messages.empty()) {
        return Result<LLMResponse>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "LLMRequest has no messages",
            .module = "Core.LLM.Echo",
        });
    }

    std::int64_t input_tokens = 0;
    for (const auto& message : request.messages) {
        input_tokens += EstimateTokens(message.content);
    }

    LLMResponse response;
    response.content = "echo: " + request.messages.back().content;
    response.stop_reason = "end_turn";
    response.usage.input_tokens = input_tokens;
    response.usage.output_tokens = EstimateTokens(response.content);
    return Result<LLMResponse>::Ok(std::move(response));
}

} // namespace aistudio::core
