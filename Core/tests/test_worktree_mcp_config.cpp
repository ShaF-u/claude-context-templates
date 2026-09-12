#include "test_framework.hpp"
#include "Core/Session/WorktreeMcpConfig.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

struct TempDir {
    fs::path path;

    TempDir() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() / fs::path("aistudio_worktree_mcp_config_test_" + std::to_string(counter++));
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempDir() { fs::remove_all(path); }

    [[nodiscard]] std::string String() const { return path.string(); }
};

Json ReadJsonFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream buffer;
    buffer << in.rdbuf();
    return Json::parse(buffer.str());
}

} // namespace

AISTUDIO_TEST(WriteWorktreeMcpConfig_WritesExpectedJsonShape) {
    TempDir dir;
    const auto result = WriteWorktreeMcpConfig(dir.String(), "C:\\repo\\build\\Core\\Debug\\aistudio_core_cli.exe");
    AISTUDIO_EXPECT(result.IsOk());

    const auto config_path = dir.path / ".mcp.json";
    AISTUDIO_EXPECT(fs::exists(config_path));

    const Json doc = ReadJsonFile(config_path);
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["type"] == "stdio");
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["command"] == "C:\\repo\\build\\Core\\Debug\\aistudio_core_cli.exe");
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["args"].is_array());
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["args"].size() == 1);
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["args"][0] == "--mcp");
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["env"].is_object());
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["env"].empty());
}

AISTUDIO_TEST(WriteWorktreeMcpConfig_OverwritesExistingFile) {
    TempDir dir;
    AISTUDIO_EXPECT(WriteWorktreeMcpConfig(dir.String(), "C:\\first\\aistudio_core_cli.exe"));
    AISTUDIO_EXPECT(WriteWorktreeMcpConfig(dir.String(), "C:\\second\\aistudio_core_cli.exe"));

    const Json doc = ReadJsonFile(dir.path / ".mcp.json");
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["command"] == "C:\\second\\aistudio_core_cli.exe");
}

AISTUDIO_TEST(WriteWorktreeMcpConfig_EmptyWorktreePath_Fails) {
    const auto result = WriteWorktreeMcpConfig("", "C:\\repo\\aistudio_core_cli.exe");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(WriteWorktreeMcpConfig_EmptyCoreCliPath_Fails) {
    TempDir dir;
    const auto result = WriteWorktreeMcpConfig(dir.String(), "");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(WriteWorktreeMcpConfig_NonexistentDirectory_Fails) {
    TempDir dir; // exists, but we point at a path inside it that doesn't
    const auto missing = dir.path / "does_not_exist";
    const auto result = WriteWorktreeMcpConfig(missing.string(), "C:\\repo\\aistudio_core_cli.exe");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(WriteWorktreeMcpConfig_PathWithSpacesAndBackslashes_RoundTripsCorrectlyEscaped) {
    TempDir dir;
    const std::string tricky_path = "C:\\Program Files\\Some App\\aistudio_core_cli.exe";
    AISTUDIO_EXPECT(WriteWorktreeMcpConfig(dir.String(), tricky_path));

    const Json doc = ReadJsonFile(dir.path / ".mcp.json");
    AISTUDIO_EXPECT(doc["mcpServers"]["aistudio-core"]["command"].get<std::string>() == tricky_path);
}
