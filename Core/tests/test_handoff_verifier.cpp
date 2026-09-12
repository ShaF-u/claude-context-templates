#include "test_framework.hpp"
#include "Core/Session/HandoffVerifier.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(RunAndVerifyCommand_SuccessfulCommand_ReportsSucceededWithOutput) {
    const auto result = RunAndVerifyCommand("cmd.exe", {"/c", "echo", "hello"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    const auto& verification = result.Value();
    AISTUDIO_EXPECT(verification.verified_outcome == HandoffVerificationOutcome::Succeeded);
    AISTUDIO_EXPECT(verification.verified_output.find("hello") != std::string::npos);
    AISTUDIO_EXPECT(verification.command.find("echo") != std::string::npos);
    AISTUDIO_EXPECT(verification.verified_at > 0);
}

AISTUDIO_TEST(RunAndVerifyCommand_NonZeroExit_ReportsFailed) {
    const auto result = RunAndVerifyCommand("cmd.exe", {"/c", "exit", "3"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().verified_outcome == HandoffVerificationOutcome::Failed);
}

AISTUDIO_TEST(RunAndVerifyCommand_NonexistentExecutable_Fails) {
    const auto result = RunAndVerifyCommand("this_does_not_exist.exe", {}, "");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(RunAndVerifyCommand_TimesOutBeforeExit_ReportsUnknown_NotFailed) {
    const auto result = RunAndVerifyCommand("cmd.exe", {"/c", "ping", "-n", "3", "127.0.0.1", ">", "NUL"}, "",
                                             std::chrono::milliseconds(50));
    AISTUDIO_EXPECT(result.IsOk());
    // Never Failed: an inconclusive/interrupted check must stay
    // distinguishable from a real failure (docs/ROADMAP.md 13-6/13-7).
    AISTUDIO_EXPECT(result.Value().verified_outcome == HandoffVerificationOutcome::Unknown);
}

AISTUDIO_TEST(RunAndVerifyCommand_ClaimedSummary_DefaultsEmpty) {
    // claimed_summary is never set by RunAndVerifyCommand itself -- it's
    // the authoring AI's own field, populated separately if at all.
    const auto result = RunAndVerifyCommand("cmd.exe", {"/c", "exit", "0"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().claimed_summary.empty());
}
