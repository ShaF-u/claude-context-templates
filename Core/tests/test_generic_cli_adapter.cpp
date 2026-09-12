#include "test_framework.hpp"
#include "Core/Session/GenericCliAdapter.hpp"

#include <chrono>
#include <string>
#include <thread>

using namespace aistudio::core;

namespace {

std::string WaitForOutputContaining(ICliAdapter& adapter, std::string_view needle,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::string accumulated;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        accumulated += adapter.TakeOutput();
        if (accumulated.find(needle) != std::string::npos) {
            return accumulated;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return accumulated;
}

} // namespace

AISTUDIO_TEST(GenericCliAdapter_Capabilities_AreAllUnknown) {
    GenericCliAdapter adapter("test-cli");
    const CliCapabilities& caps = adapter.Capabilities();
    AISTUDIO_EXPECT(caps.resume_conversation == SupportLevel::Unknown);
    AISTUDIO_EXPECT(caps.cancel == SupportLevel::Unknown);
    AISTUDIO_EXPECT(caps.approval_wait_detection == SupportLevel::Unknown);
    AISTUDIO_EXPECT(caps.structured_output == SupportLevel::Unknown);
    AISTUDIO_EXPECT(caps.usage_retrieval == SupportLevel::Unknown);
}

AISTUDIO_TEST(GenericCliAdapter_Name_ReturnsWhatWasPassedIn) {
    GenericCliAdapter adapter("claude");
    AISTUDIO_EXPECT(adapter.Name() == "claude");
}

AISTUDIO_TEST(GenericCliAdapter_Start_SpawnsRealChildAndCapturesOutput) {
    GenericCliAdapter adapter("test-cli");
    CliProfile profile;
    profile.command_line = L"cmd.exe /c echo hello_from_adapter";
    AISTUDIO_EXPECT(adapter.Start(profile));
    const std::string output = WaitForOutputContaining(adapter, "hello_from_adapter");
    AISTUDIO_EXPECT(output.find("hello_from_adapter") != std::string::npos);
    adapter.Stop();
}

AISTUDIO_TEST(GenericCliAdapter_Start_NonexistentExecutable_FailsWithMessage) {
    GenericCliAdapter adapter("test-cli");
    CliProfile profile;
    profile.command_line = L"this_executable_does_not_exist_anywhere.exe";
    AISTUDIO_EXPECT(!adapter.Start(profile));
    AISTUDIO_EXPECT(!adapter.LastErrorMessage().empty());
}

AISTUDIO_TEST(GenericCliAdapter_Start_EnvironmentVariables_ArePassedToChild) {
    GenericCliAdapter adapter("test-cli");
    CliProfile profile;
    profile.command_line = L"cmd.exe /c echo %AISTUDIO_ADAPTER_TEST_VAR%";
    profile.environment_variables = {{L"AISTUDIO_ADAPTER_TEST_VAR", L"adapter_env_value"}};
    AISTUDIO_EXPECT(adapter.Start(profile));
    const std::string output = WaitForOutputContaining(adapter, "adapter_env_value");
    AISTUDIO_EXPECT(output.find("adapter_env_value") != std::string::npos);
    adapter.Stop();
}

AISTUDIO_TEST(GenericCliAdapter_Start_WorkingDirectory_IsApplied) {
    GenericCliAdapter adapter("test-cli");
    CliProfile profile;
    profile.command_line = L"cmd.exe /c cd";
    profile.working_directory = "C:\\Windows";
    AISTUDIO_EXPECT(adapter.Start(profile));
    const std::string output = WaitForOutputContaining(adapter, "Windows");
    AISTUDIO_EXPECT(output.find("Windows") != std::string::npos);
    adapter.Stop();
}

AISTUDIO_TEST(GenericCliAdapter_SendInput_IsEchoedBackByInteractiveShell) {
    GenericCliAdapter adapter("test-cli");
    CliProfile profile;
    profile.command_line = L"cmd.exe";
    AISTUDIO_EXPECT(adapter.Start(profile));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::string discarded_banner = adapter.TakeOutput();
    (void)discarded_banner;
    adapter.SendInput("echo roundtrip_check\r\n");
    const std::string output = WaitForOutputContaining(adapter, "roundtrip_check");
    AISTUDIO_EXPECT(output.find("roundtrip_check") != std::string::npos);
    adapter.Stop();
}

AISTUDIO_TEST(GenericCliAdapter_Stop_ActuallyStopsChild) {
    GenericCliAdapter adapter("test-cli");
    CliProfile profile;
    profile.command_line = L"cmd.exe /c ping -n 10 127.0.0.1 > NUL";
    AISTUDIO_EXPECT(adapter.Start(profile));
    AISTUDIO_EXPECT(adapter.IsRunning());
    adapter.Stop();
    AISTUDIO_EXPECT(!adapter.IsRunning());
}
