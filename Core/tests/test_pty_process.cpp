#include "test_framework.hpp"
#include "Core/Terminal/PtyProcess.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace aistudio::core;

namespace {

// Polls TakeOutput() (the only way to observe PTY output -- it's drained
// asynchronously by a background reader thread, same reasoning as
// ManagedProcess::OutputSoFar() but destructive/accumulating here) until
// `needle` appears in the accumulated text or `timeout` elapses.
std::string WaitForOutputContaining(PtyProcess& process, std::string_view needle,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::string accumulated;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        accumulated += process.TakeOutput();
        if (accumulated.find(needle) != std::string::npos) {
            return accumulated;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return accumulated;
}

} // namespace

AISTUDIO_TEST(PtyProcess_Start_SpawnsRealChildAndReportsRunning) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe"));
    AISTUDIO_EXPECT(process.IsRunning());
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_Start_ExtraEnvironment_OverridesVariableAndKeepsInheritedOnes) {
    PtyProcess process;
    const std::vector<std::pair<std::wstring, std::wstring>> extra_env = {
        {L"AISTUDIO_TEST_VAR", L"hello123"},
    };
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c echo %AISTUDIO_TEST_VAR% & echo PATH_IS:%PATH%", 120, 32, extra_env));
    const std::string output = WaitForOutputContaining(process, "PATH_IS:");
    AISTUDIO_EXPECT(output.find("hello123") != std::string::npos);
    // PATH is inherited from this test process's own environment, not
    // wiped out by only supplying AISTUDIO_TEST_VAR above -- if it were
    // wiped, cmd.exe would echo the literal "%PATH%" unexpanded.
    AISTUDIO_EXPECT(output.find("PATH_IS:") != std::string::npos);
    AISTUDIO_EXPECT(output.find("PATH_IS:%PATH%") == std::string::npos);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_Resize_ChangesReportedConsoleColumns) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe", 80, 24));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::string discarded_banner = process.TakeOutput();
    (void)discarded_banner;

    process.Resize(101, 40);
    process.WriteInput("mode con\r\n");
    const std::string output = WaitForOutputContaining(process, "101");
    AISTUDIO_EXPECT(output.find("101") != std::string::npos);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_Start_NonexistentExecutable_FailsWithMessage) {
    PtyProcess process;
    AISTUDIO_EXPECT(!process.Start(L"this_executable_does_not_exist_anywhere.exe"));
    AISTUDIO_EXPECT(!process.LastErrorMessage().empty());
    AISTUDIO_EXPECT(!process.IsRunning());
}

AISTUDIO_TEST(PtyProcess_Start_WhileAlreadyRunning_Fails) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe"));
    AISTUDIO_EXPECT(!process.Start(L"cmd.exe")); // already running -- second Start() is a no-op failure
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_TakeOutput_CapturesRealChildOutput) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c echo hello_pty_test"));
    const std::string output = WaitForOutputContaining(process, "hello_pty_test");
    AISTUDIO_EXPECT(output.find("hello_pty_test") != std::string::npos);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_TakeOutput_DrainsAndClearsBuffer) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c echo drain_test"));
    WaitForOutputContaining(process, "drain_test");
    // The first TakeOutput() inside WaitForOutputContaining() (or a
    // subsequent poll) already drained everything containing the needle
    // -- a fresh call right after must not re-return the same bytes.
    const std::string second = process.TakeOutput();
    AISTUDIO_EXPECT(second.find("drain_test") == std::string::npos);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_WriteInput_IsEchoedBackByInteractiveShell) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe"));
    // Give the shell a moment to print its banner/prompt before sending
    // input, same real-process-latency reasoning test_process_runner.cpp
    // uses for interactive commands.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::string discarded_banner = process.TakeOutput();
    (void)discarded_banner;
    process.WriteInput("echo raw_input_roundtrip\r\n");
    const std::string output = WaitForOutputContaining(process, "raw_input_roundtrip");
    AISTUDIO_EXPECT(output.find("raw_input_roundtrip") != std::string::npos);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_Stop_ActuallyStopsTheChildProcess) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c ping -n 10 127.0.0.1 > NUL"));
    AISTUDIO_EXPECT(process.IsRunning());
    process.Stop();
    AISTUDIO_EXPECT(!process.IsRunning());
}

AISTUDIO_TEST(PtyProcess_IsRunning_BecomesFalseAfterChildExitsOnItsOwn) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c exit 0"));
    bool became_not_running = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process.IsRunning()) {
            became_not_running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AISTUDIO_EXPECT(became_not_running);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_ExitCode_IsNulloptWhileRunningAndNeverStarted) {
    PtyProcess process;
    AISTUDIO_EXPECT(!process.ExitCode().has_value());
    AISTUDIO_EXPECT(process.Start(L"cmd.exe"));
    AISTUDIO_EXPECT(!process.ExitCode().has_value());
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_ExitCode_ReflectsOrganicNonzeroExit) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c exit 3"));
    bool became_not_running = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process.IsRunning()) {
            became_not_running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AISTUDIO_EXPECT(became_not_running);
    AISTUDIO_EXPECT(process.ExitCode().has_value());
    AISTUDIO_EXPECT(process.ExitCode().value() == 3);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_ExitCode_StopOnStillRunningChild_IsZero) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c ping -n 10 127.0.0.1 > NUL"));
    AISTUDIO_EXPECT(process.IsRunning());
    process.Stop();
    AISTUDIO_EXPECT(process.ExitCode().has_value());
    AISTUDIO_EXPECT(process.ExitCode().value() == 0);
}

AISTUDIO_TEST(PtyProcess_Start_AfterChildExitedOnItsOwn_SucceedsRatherThanTerminating) {
    PtyProcess process;
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c exit 0"));
    bool became_not_running = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!process.IsRunning()) {
            became_not_running = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AISTUDIO_EXPECT(became_not_running);
    // Reaching this line at all is the assertion: reusing this PtyProcess
    // after a natural exit used to move-assign onto a still-joinable
    // reader_thread and call std::terminate(), aborting the process.
    AISTUDIO_EXPECT(process.Start(L"cmd.exe /c echo still_alive"));
    const std::string output = WaitForOutputContaining(process, "still_alive");
    AISTUDIO_EXPECT(output.find("still_alive") != std::string::npos);
    process.Stop();
}

AISTUDIO_TEST(PtyProcess_Destructor_StopsStillRunningChildWithoutHanging) {
    // The child (~2s of runtime) must not still be running once this
    // scope exits and ~PtyProcess() runs -- same reasoning as
    // StartProcess_DroppedWithoutWait_TerminatesRatherThanLeavingAnOrphan
    // in test_process_runner.cpp. If this hangs, the destructor is
    // failing to terminate + join the reader thread correctly.
    {
        PtyProcess process;
        AISTUDIO_EXPECT(process.Start(L"cmd.exe /c ping -n 10 127.0.0.1 > NUL"));
        AISTUDIO_EXPECT(process.IsRunning());
    }
    // Reaching this line at all (rather than the test hanging/timing out)
    // is the actual assertion.
    AISTUDIO_EXPECT(true);
}
