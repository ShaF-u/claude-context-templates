#include "Core/Session/HandoffVerifier.hpp"

#include "Core/Util/ProcessRunner.hpp"
#include "Core/Util/Time.hpp"

namespace aistudio::core {

namespace {

std::string JoinCommandForDisplay(const std::string& executable, const std::vector<std::string>& args) {
    std::string result = executable;
    for (const auto& arg : args) {
        result += ' ';
        result += arg;
    }
    return result;
}

} // namespace

Result<HandoffVerification> RunAndVerifyCommand(const std::string& executable, const std::vector<std::string>& args,
                                                  const std::string& working_directory,
                                                  std::optional<std::chrono::milliseconds> timeout) {
    auto started = StartProcess(executable, args, working_directory);
    if (!started) {
        return Result<HandoffVerification>::Fail(started.Err());
    }

    HandoffVerification verification;
    verification.command = JoinCommandForDisplay(executable, args);

    const auto exit_code = started.Value().Wait(timeout);
    verification.verified_output = started.Value().OutputSoFar();
    verification.verified_at = CurrentUnixTimestamp();
    if (!exit_code.has_value()) {
        verification.verified_outcome = HandoffVerificationOutcome::Unknown;
    } else if (*exit_code == 0) {
        verification.verified_outcome = HandoffVerificationOutcome::Succeeded;
    } else {
        verification.verified_outcome = HandoffVerificationOutcome::Failed;
    }

    return Result<HandoffVerification>::Ok(std::move(verification));
}

} // namespace aistudio::core
