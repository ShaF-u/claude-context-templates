#include "test_framework.hpp"
#include "Core/Session/HandoffRecord.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(HandoffVerificationOutcome_ToString_MatchesExpectedNames) {
    AISTUDIO_EXPECT(ToString(HandoffVerificationOutcome::Unknown) == "Unknown");
    AISTUDIO_EXPECT(ToString(HandoffVerificationOutcome::Succeeded) == "Succeeded");
    AISTUDIO_EXPECT(ToString(HandoffVerificationOutcome::Failed) == "Failed");
}

AISTUDIO_TEST(HandoffVerificationOutcomeFromString_RoundTripsToString) {
    AISTUDIO_EXPECT(HandoffVerificationOutcomeFromString("Succeeded") == HandoffVerificationOutcome::Succeeded);
    AISTUDIO_EXPECT(HandoffVerificationOutcomeFromString("Failed") == HandoffVerificationOutcome::Failed);
    AISTUDIO_EXPECT(HandoffVerificationOutcomeFromString("Unknown") == HandoffVerificationOutcome::Unknown);
}

AISTUDIO_TEST(HandoffVerificationOutcomeFromString_UnrecognizedText_DefaultsToUnknown) {
    AISTUDIO_EXPECT(HandoffVerificationOutcomeFromString("garbage") == HandoffVerificationOutcome::Unknown);
    AISTUDIO_EXPECT(HandoffVerificationOutcomeFromString("") == HandoffVerificationOutcome::Unknown);
}
