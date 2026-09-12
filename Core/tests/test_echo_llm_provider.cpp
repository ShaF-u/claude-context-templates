#include "test_framework.hpp"
#include "Core/LLM/EchoLLMProvider.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(ChatRole_ToString_CoversAllValues) {
    AISTUDIO_EXPECT(ToString(ChatRole::System) == "System");
    AISTUDIO_EXPECT(ToString(ChatRole::User) == "User");
    AISTUDIO_EXPECT(ToString(ChatRole::Assistant) == "Assistant");
}

AISTUDIO_TEST(EchoLLMProvider_Name_IsEcho) {
    const EchoLLMProvider provider;
    AISTUDIO_EXPECT(provider.Name() == "echo");
}

AISTUDIO_TEST(EchoLLMProvider_Complete_EchoesLastUserMessage) {
    EchoLLMProvider provider;
    LLMRequest request;
    request.messages.push_back(ChatMessage{ChatRole::User, "hello"});

    const auto result = provider.Complete(request);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().content == "echo: hello");
}

AISTUDIO_TEST(EchoLLMProvider_Complete_UsesLastMessageNotFirst) {
    EchoLLMProvider provider;
    LLMRequest request;
    request.messages.push_back(ChatMessage{ChatRole::System, "you are helpful"});
    request.messages.push_back(ChatMessage{ChatRole::User, "first"});
    request.messages.push_back(ChatMessage{ChatRole::User, "second"});

    const auto result = provider.Complete(request);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().content == "echo: second");
}

AISTUDIO_TEST(EchoLLMProvider_Complete_EmptyMessages_Fails) {
    EchoLLMProvider provider;
    const auto result = provider.Complete(LLMRequest{});
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(EchoLLMProvider_Complete_SetsStopReason) {
    EchoLLMProvider provider;
    LLMRequest request;
    request.messages.push_back(ChatMessage{ChatRole::User, "hi"});

    const auto result = provider.Complete(request);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().stop_reason == "end_turn");
}

AISTUDIO_TEST(EchoLLMProvider_Complete_TracksTokenUsage) {
    EchoLLMProvider provider;
    LLMRequest request;
    request.messages.push_back(ChatMessage{ChatRole::User, "hello world"});

    const auto result = provider.Complete(request);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value().usage.input_tokens > 0);
    AISTUDIO_EXPECT(result.Value().usage.output_tokens > 0);
}

AISTUDIO_TEST(EchoLLMProvider_CompleteStreaming_DeliversWholeContentAsOneChunk) {
    EchoLLMProvider provider;
    LLMRequest request;
    request.messages.push_back(ChatMessage{ChatRole::User, "hello"});

    std::vector<std::string> chunks;
    const auto result = provider.CompleteStreaming(request, [&](const std::string& chunk) { chunks.push_back(chunk); });

    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(chunks.size() == 1);
    AISTUDIO_EXPECT(chunks.front() == "echo: hello");
}

AISTUDIO_TEST(EchoLLMProvider_CompleteStreaming_Fails_NoChunkDelivered) {
    EchoLLMProvider provider;
    std::vector<std::string> chunks;
    const auto result = provider.CompleteStreaming(LLMRequest{}, [&](const std::string& chunk) { chunks.push_back(chunk); });

    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(chunks.empty());
}
