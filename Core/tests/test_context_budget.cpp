#include "test_framework.hpp"
#include "Core/Context/ContextBudget.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(ContextBudget_CanFit_WithinRemaining) {
    ContextBudget budget(100);
    AISTUDIO_EXPECT(budget.CanFit(100));
    AISTUDIO_EXPECT(!budget.CanFit(101));
}

AISTUDIO_TEST(ContextBudget_Charge_Succeeds_UpdatesUsedAndRemaining) {
    ContextBudget budget(100);
    AISTUDIO_EXPECT(budget.Charge(40));
    AISTUDIO_EXPECT(budget.UsedTokens() == 40);
    AISTUDIO_EXPECT(budget.RemainingTokens() == 60);
}

AISTUDIO_TEST(ContextBudget_Charge_ExceedsRemaining_FailsAndLeavesUnchanged) {
    ContextBudget budget(100);
    budget.Charge(90);
    AISTUDIO_EXPECT(!budget.Charge(20));
    AISTUDIO_EXPECT(budget.UsedTokens() == 90);
}

AISTUDIO_TEST(ContextBudget_Reset_ClearsUsage) {
    ContextBudget budget(100);
    budget.Charge(50);
    budget.Reset();
    AISTUDIO_EXPECT(budget.UsedTokens() == 0);
    AISTUDIO_EXPECT(budget.RemainingTokens() == 100);
}
