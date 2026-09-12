#include "test_framework.hpp"
#include "Core/Config/Config.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace aistudio::core;

AISTUDIO_TEST(Config_SetGet_RoundTrips) {
    Config config;
    config.Set("studio.name", "Test Studio");
    AISTUDIO_EXPECT(config.Get("studio.name").value() == "Test Studio");
}

AISTUDIO_TEST(Config_GetOr_UsesFallbackWhenMissing) {
    Config config;
    AISTUDIO_EXPECT(config.GetOr("missing.key", "fallback") == "fallback");
}

AISTUDIO_TEST(Config_LoadFromFile_MissingFile_ReturnsError) {
    Config config;
    const auto result = config.LoadFromFile("this_file_does_not_exist.ini");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(Config_SetSecretGetSecret_RoundTrips) {
    Config config;
    config.SetSecret("llm.api_key", "sk-super-secret");
    const auto secret = config.GetSecret("llm.api_key");
    AISTUDIO_EXPECT(secret.has_value());
    AISTUDIO_EXPECT(secret->Reveal() == "sk-super-secret");
}

AISTUDIO_TEST(Config_GetSecret_UnknownKey_ReturnsNullopt) {
    Config config;
    AISTUDIO_EXPECT(!config.GetSecret("does.not.exist").has_value());
}

AISTUDIO_TEST(Config_Get_DoesNotReturnSecretValues) {
    Config config;
    config.SetSecret("llm.api_key", "sk-super-secret");
    AISTUDIO_EXPECT(!config.Get("llm.api_key").has_value());
}

AISTUDIO_TEST(Config_IsSecret_ReflectsHowKeyWasSet) {
    Config config;
    config.Set("studio.name", "Test Studio");
    config.SetSecret("llm.api_key", "sk-super-secret");
    AISTUDIO_EXPECT(!config.IsSecret("studio.name"));
    AISTUDIO_EXPECT(config.IsSecret("llm.api_key"));
}

AISTUDIO_TEST(Config_SetSecret_ThenSet_DemotesToPlain) {
    Config config;
    config.SetSecret("key", "secret-value");
    config.Set("key", "plain-value");
    AISTUDIO_EXPECT(!config.IsSecret("key"));
    AISTUDIO_EXPECT(config.Get("key").value() == "plain-value");
    AISTUDIO_EXPECT(!config.GetSecret("key").has_value());
}

AISTUDIO_TEST(Config_Set_ThenSetSecret_PromotesToSecret) {
    Config config;
    config.Set("key", "plain-value");
    config.SetSecret("key", "secret-value");
    AISTUDIO_EXPECT(config.IsSecret("key"));
    AISTUDIO_EXPECT(!config.Get("key").has_value());
}

AISTUDIO_TEST(Config_SaveToFile_NeverPersistsSecretValues) {
    namespace fs = std::filesystem;
    static std::atomic<int> counter{0};
    const auto path = fs::temp_directory_path() / ("aistudio_config_secret_test_" + std::to_string(counter++) + ".ini");

    Config config;
    config.Set("studio.name", "Test Studio");
    config.SetSecret("llm.api_key", "sk-super-secret");
    AISTUDIO_EXPECT(config.SaveToFile(path.string()));

    std::ostringstream buffer;
    {
        std::ifstream in(path);
        buffer << in.rdbuf();
    } // closed before fs::remove() below — Windows locks open file handles
    const auto contents = buffer.str();

    AISTUDIO_EXPECT(contents.find("studio.name") != std::string::npos);
    AISTUDIO_EXPECT(contents.find("sk-super-secret") == std::string::npos);
    AISTUDIO_EXPECT(contents.find("llm.api_key") == std::string::npos);

    fs::remove(path);
}
