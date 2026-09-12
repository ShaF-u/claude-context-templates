#include "test_framework.hpp"
#include "Core/Security/Secret.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(Secret_Reveal_ReturnsTheRealValue) {
    const Secret secret("sk-super-secret");
    AISTUDIO_EXPECT(secret.Reveal() == "sk-super-secret");
}

AISTUDIO_TEST(Secret_ToString_NeverReturnsTheRealValue) {
    const Secret secret("sk-super-secret");
    AISTUDIO_EXPECT(secret.ToString() != "sk-super-secret");
    AISTUDIO_EXPECT(secret.ToString().find("sk-super-secret") == std::string::npos);
}

AISTUDIO_TEST(Secret_ToString_IsRedactionMarker) {
    const Secret secret("anything");
    AISTUDIO_EXPECT(secret.ToString() == "***REDACTED***");
}

AISTUDIO_TEST(Secret_Empty_DefaultConstructed_IsTrue) {
    const Secret secret;
    AISTUDIO_EXPECT(secret.Empty());
}

AISTUDIO_TEST(Secret_Empty_WithValue_IsFalse) {
    const Secret secret("value");
    AISTUDIO_EXPECT(!secret.Empty());
}

AISTUDIO_TEST(Secret_ToString_EmptySecret_IsEmptyNotRedactionMarker) {
    const Secret secret;
    AISTUDIO_EXPECT(secret.ToString().empty());
}
