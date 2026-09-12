#include "test_framework.hpp"
#include "Core/Session/CliDiagnostics.hpp"
#include "Core/Util/Utf8.hpp"

#include <filesystem>

using namespace aistudio::core;

namespace {
namespace fs = std::filesystem;
} // namespace

AISTUDIO_TEST(DiagnoseCliAvailability_BareNameFoundInPath_ReturnsAvailable) {
    CliProfile profile;
    profile.command_line = L"cmd.exe";
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::Available);
    AISTUDIO_EXPECT(!result.resolved_path.empty());
    AISTUDIO_EXPECT(result.resolved_path.find("cmd.exe") != std::string::npos);
}

AISTUDIO_TEST(DiagnoseCliAvailability_BareNameNotInPath_ReturnsNotFound) {
    CliProfile profile;
    profile.command_line = L"this_cli_definitely_does_not_exist_12345.exe";
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::NotFound);
    AISTUDIO_EXPECT(result.resolved_path.empty());
    AISTUDIO_EXPECT(!result.message.empty());
}

AISTUDIO_TEST(DiagnoseCliAvailability_AbsolutePathToRealExecutable_ReturnsAvailable) {
    CliProfile profile;
    profile.command_line = Utf8ToWide(AISTUDIO_ARGV_ECHO_FIXTURE_PATH);
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::Available);
    AISTUDIO_EXPECT(!result.resolved_path.empty());
}

AISTUDIO_TEST(DiagnoseCliAvailability_AbsolutePathNotFound_ReturnsNotFound) {
    CliProfile profile;
    profile.command_line = L"C:\\this\\path\\definitely\\does\\not\\exist\\fake.exe";
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::NotFound);
}

AISTUDIO_TEST(DiagnoseCliAvailability_QuotedExecutablePathWithArgs_ExtractsTokenCorrectly) {
    CliProfile profile;
    profile.command_line = L"\"" + Utf8ToWide(AISTUDIO_ARGV_ECHO_FIXTURE_PATH) + L"\" one two three";
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::Available);
}

AISTUDIO_TEST(DiagnoseCliAvailability_UnquotedBareNameWithArgs_ExtractsFirstTokenOnly) {
    CliProfile profile;
    profile.command_line = L"cmd.exe /c echo hello";
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::Available);
}

AISTUDIO_TEST(DiagnoseCliAvailability_EmptyCommandLine_ReturnsNotFound) {
    CliProfile profile;
    profile.command_line = L"";
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::NotFound);
}

AISTUDIO_TEST(DiagnoseCliAvailability_RelativePathResolvesAgainstWorkingDirectory) {
    const fs::path fixture_path(Utf8ToWide(AISTUDIO_ARGV_ECHO_FIXTURE_PATH));
    CliProfile profile;
    profile.working_directory = fixture_path.parent_path().string();
    profile.command_line = L".\\" + fixture_path.filename().wstring();
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::Available);
}

AISTUDIO_TEST(DiagnoseCliAvailability_ExplicitPathWithoutExtension_StillResolvesWithExeAppended) {
    const fs::path fixture_path(Utf8ToWide(AISTUDIO_ARGV_ECHO_FIXTURE_PATH));
    CliProfile profile;
    // Same path, minus its ".exe" extension -- exercises the "no
    // extension given, try appending .exe" branch of an explicit path.
    profile.command_line = fixture_path.parent_path().wstring() + L"\\" + fixture_path.stem().wstring();
    const auto result = DiagnoseCliAvailability(profile);
    AISTUDIO_EXPECT(result.availability == CliAvailability::Available);
}
