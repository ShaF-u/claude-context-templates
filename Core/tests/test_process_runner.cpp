#include "test_framework.hpp"
#include "Core/Util/ProcessRunner.hpp"

#include <filesystem>
#include <sstream>
#include <vector>

using namespace aistudio::core;

AISTUDIO_TEST(RunProcess_CapturesStdout) {
    const auto result = RunProcess("cmd.exe", {"/c", "echo", "hello"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().exit_code == 0);
    AISTUDIO_EXPECT(result.Value().output.find("hello") != std::string::npos);
}

AISTUDIO_TEST(RunProcess_NonexistentExecutable_Fails) {
    const auto result = RunProcess("this_executable_does_not_exist_anywhere.exe", {}, "");
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::IOError);
}

AISTUDIO_TEST(RunProcess_NonZeroExitCode_IsReportedNotFailed) {
    const auto result = RunProcess("cmd.exe", {"/c", "exit", "3"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().exit_code == 3);
}

AISTUDIO_TEST(RunProcess_WorkingDirectory_IsApplied) {
    const std::string temp_dir = std::filesystem::temp_directory_path().string();
    const auto result = RunProcess("cmd.exe", {"/c", "cd"}, temp_dir);
    AISTUDIO_EXPECT(result.IsOk());
    // cmd's own `cd` output for a path like "C:\Users\x\AppData\Local\Temp\"
    // starts with the drive letter; just check the reported directory
    // shares a long enough prefix (drop any trailing separator both sides
    // might differ on) rather than requiring an exact string match.
    std::string expected_prefix = temp_dir;
    while (!expected_prefix.empty() && (expected_prefix.back() == '\\' || expected_prefix.back() == '/')) {
        expected_prefix.pop_back();
    }
    AISTUDIO_EXPECT(result.Value().output.find(expected_prefix) != std::string::npos);
}

namespace {

// Splits aistudio_argv_echo_fixture's "<count>\x1f<arg1>\x1f<arg2>..."
// output back into the exact argv strings the fixture process actually
// received, so a test can compare against what RunProcess was asked to
// send -- proving BuildCommandLine/QuoteArg (Core/src/Util/
// ProcessRunner.cpp) round-trip adversarial content through a REAL
// CreateProcessW boundary correctly, not just by inspection of the
// quoting algorithm.
std::vector<std::string> ParseArgvEchoOutput(const std::string& output) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (true) {
        const auto sep = output.find('\x1f', start);
        parts.push_back(output.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return parts; // parts[0] is the fixture's own reported argc-1
}

std::vector<std::string> EchoArgs(const std::vector<std::string>& args) {
    const auto result = RunProcess(AISTUDIO_ARGV_ECHO_FIXTURE_PATH, args, "");
    AISTUDIO_EXPECT(result.IsOk());
    if (!result) return {};
    auto parts = ParseArgvEchoOutput(result.Value().output);
    parts.erase(parts.begin()); // drop the leading argc-1 field
    return parts;
}

} // namespace

// The classic Windows command-line injection shape: an argument whose
// content, if the quoting were naive, could make the child process's own
// argv parser see an embedded quote as closing the argument early and
// start treating what follows as SEPARATE arguments (effectively
// "escaping" the single logical argument RunProcess's caller intended --
// e.g. GitBackend passing an MCP-client-controlled `intent` string
// straight into `"--grep=" + intent`, docs/ROADMAP.md "ProcessRunnerの
// クォート処理の監査").
AISTUDIO_TEST(RunProcess_ArgWithEmbeddedQuoteAndSpaces_ArrivesAsOneArgument) {
    const auto received = EchoArgs({"before", "foo\" --evil \"bar", "after"});
    AISTUDIO_EXPECT(received.size() == 3);
    AISTUDIO_EXPECT(received[0] == "before");
    AISTUDIO_EXPECT(received[1] == "foo\" --evil \"bar");
    AISTUDIO_EXPECT(received[2] == "after");
}

AISTUDIO_TEST(RunProcess_ArgWithTrailingBackslashBeforeEnd_IsPreservedExactly) {
    // A naive quoter that doesn't double a backslash immediately
    // preceding the closing quote lets the backslash escape that quote
    // instead, corrupting this argument and merging it with the next one.
    const auto received = EchoArgs({"C:\\some\\path\\", "next"});
    AISTUDIO_EXPECT(received.size() == 2);
    AISTUDIO_EXPECT(received[0] == "C:\\some\\path\\");
    AISTUDIO_EXPECT(received[1] == "next");
}

AISTUDIO_TEST(RunProcess_ArgWithSpaces_ArrivesAsOneArgument) {
    const auto received = EchoArgs({"hello world", "second arg"});
    AISTUDIO_EXPECT(received.size() == 2);
    AISTUDIO_EXPECT(received[0] == "hello world");
    AISTUDIO_EXPECT(received[1] == "second arg");
}

AISTUDIO_TEST(RunProcess_EmptyStringArg_ArrivesAsOneEmptyArgument) {
    const auto received = EchoArgs({"before", "", "after"});
    AISTUDIO_EXPECT(received.size() == 3);
    AISTUDIO_EXPECT(received[1].empty());
}

AISTUDIO_TEST(RunProcess_ArgThatIsOnlyBackslashes_IsPreservedExactly) {
    const auto received = EchoArgs({"\\\\\\\\"});
    AISTUDIO_EXPECT(received.size() == 1);
    AISTUDIO_EXPECT(received[0] == "\\\\\\\\");
}

// docs/ROADMAP.md Phase 13 "着手前に潰すべき前提": RunProcess's complete
// synchronicity and lack of a PID/handle/cancellation token make it
// unusable for supervising a long-running process. These test the
// non-blocking StartProcess()/ManagedProcess counterpart added to close
// that gap.

AISTUDIO_TEST(StartProcess_ReturnsRunningProcessWithNonZeroPid) {
    auto result = StartProcess("cmd.exe", {"/c", "exit", "0"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().Pid() != 0);
    AISTUDIO_EXPECT(result.Value().Wait().value() == 0);
}

AISTUDIO_TEST(StartProcess_Wait_ReturnsExitCode) {
    auto result = StartProcess("cmd.exe", {"/c", "exit", "3"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    const auto exit_code = result.Value().Wait();
    AISTUDIO_EXPECT(exit_code.has_value());
    AISTUDIO_EXPECT(*exit_code == 3);
}

AISTUDIO_TEST(StartProcess_Wait_ReturnsCachedExitCodeOnSecondCall) {
    auto result = StartProcess("cmd.exe", {"/c", "exit", "5"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    auto& process = result.Value();
    AISTUDIO_EXPECT(process.Wait().value() == 5);
    // Second call must not re-wait on an already-closed wait -- returns
    // the cached result instead of e.g. blocking or erroring.
    AISTUDIO_EXPECT(process.Wait().value() == 5);
}

AISTUDIO_TEST(StartProcess_IsRunning_TrueWhileRunning_FalseAfterWait) {
    // ~2 real seconds of runtime with no stdin needed, unlike `timeout`
    // (which prompts and would hang waiting on stdin here).
    auto result = StartProcess("cmd.exe", {"/c", "ping", "-n", "3", "127.0.0.1", ">", "NUL"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    auto& process = result.Value();
    AISTUDIO_EXPECT(process.IsRunning());
    const auto exit_code = process.Wait();
    AISTUDIO_EXPECT(exit_code.has_value());
    AISTUDIO_EXPECT(!process.IsRunning());
}

AISTUDIO_TEST(StartProcess_Wait_WithShortTimeout_ReturnsNulloptWhileStillRunning) {
    auto result = StartProcess("cmd.exe", {"/c", "ping", "-n", "3", "127.0.0.1", ">", "NUL"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    auto& process = result.Value();
    const auto early = process.Wait(std::chrono::milliseconds(50));
    AISTUDIO_EXPECT(!early.has_value());
    // Cleans up rather than leaving a ~2s process running past the test.
    process.Terminate();
}

AISTUDIO_TEST(StartProcess_Terminate_StopsTheProcess) {
    auto result = StartProcess("cmd.exe", {"/c", "ping", "-n", "10", "127.0.0.1", ">", "NUL"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    auto& process = result.Value();
    AISTUDIO_EXPECT(process.IsRunning());
    process.Terminate();
    AISTUDIO_EXPECT(!process.IsRunning());
}

AISTUDIO_TEST(StartProcess_OutputSoFar_CapturesRealOutput) {
    auto result = StartProcess("cmd.exe", {"/c", "echo", "hello"}, "");
    AISTUDIO_EXPECT(result.IsOk());
    auto& process = result.Value();
    const auto exit_code = process.Wait();
    AISTUDIO_EXPECT(exit_code.has_value());
    AISTUDIO_EXPECT(process.OutputSoFar().find("hello") != std::string::npos);
}

AISTUDIO_TEST(StartProcess_NonexistentExecutable_Fails) {
    const auto result = StartProcess("this_executable_does_not_exist_anywhere.exe", {}, "");
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::IOError);
}

AISTUDIO_TEST(StartProcess_DroppedWithoutWait_TerminatesRatherThanLeavingAnOrphan) {
    // The process this test starts (~2s of runtime) must not still be
    // running once this scope exits and ~ManagedProcess() runs -- there's
    // no direct way to assert "no orphan" from here, but if the
    // destructor's cleanup hung or threw, this test itself would hang or
    // crash rather than complete.
    { auto result = StartProcess("cmd.exe", {"/c", "ping", "-n", "10", "127.0.0.1", ">", "NUL"}, ""); }
    AISTUDIO_EXPECT(true);
}
