#include "Core/Context/ContextBudget.hpp"

namespace aistudio::core {

bool ContextBudget::Charge(std::int64_t tokens) {
    if (!CanFit(tokens)) {
        return false;
    }
    used_tokens_ += tokens;
    return true;
}

void ContextBudget::Reset() {
    used_tokens_ = 0;
}

} // namespace aistudio::core
