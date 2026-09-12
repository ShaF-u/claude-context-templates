#pragma once

#include <cstdint>

namespace aistudio::core {

// Tracks how much of the LLM's context window has been spent so a
// selection never exceeds it (docs/MASTER_SPEC.md #17 Context Budget
// Manager).
class ContextBudget {
public:
    explicit ContextBudget(std::int64_t max_tokens) : max_tokens_(max_tokens) {}

    [[nodiscard]] std::int64_t MaxTokens() const { return max_tokens_; }
    [[nodiscard]] std::int64_t UsedTokens() const { return used_tokens_; }
    [[nodiscard]] std::int64_t RemainingTokens() const { return max_tokens_ - used_tokens_; }

    [[nodiscard]] bool CanFit(std::int64_t tokens) const { return tokens <= RemainingTokens(); }

    // Returns false (and leaves the budget unchanged) if `tokens` would
    // exceed what remains.
    bool Charge(std::int64_t tokens);

    void Reset();

private:
    std::int64_t max_tokens_;
    std::int64_t used_tokens_ = 0;
};

} // namespace aistudio::core
