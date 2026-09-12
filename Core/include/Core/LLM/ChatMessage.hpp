#pragma once

#include <string>

namespace aistudio::core {

// Who a ChatMessage is attributed to — the common three-role shape every
// major chat-completion API (Anthropic, OpenAI, ...) shares, so this
// stays provider-agnostic rather than mirroring one vendor's schema
// (docs/ROADMAP.md Phase 4 "LLM Integration" — the whole point is that
// swapping providers doesn't touch call sites built on this).
enum class ChatRole { System, User, Assistant };

[[nodiscard]] std::string ToString(ChatRole role);

struct ChatMessage {
    ChatRole role = ChatRole::User;
    std::string content;
};

} // namespace aistudio::core
