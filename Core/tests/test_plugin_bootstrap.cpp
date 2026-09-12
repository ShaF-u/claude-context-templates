#include "test_framework.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Plugin/PluginBootstrap.hpp"
#include "Core/Plugin/PluginRegistry.hpp"

using namespace aistudio::core;

// Exercises LoadConfiguredPluginBackends() against the real
// aistudio_plugin_fixture/aistudio_plugin_fixture_broken DLLs (same
// fixtures test_plugin_backend_adapter.cpp uses -- see its own comment),
// so this is real DLL loading through Config + PluginLoader, not a mock.

AISTUDIO_TEST(LoadConfiguredPluginBackends_EmptyConfig_ReturnsEmpty) {
    Config config;
    PluginLoader loader;
    AISTUDIO_EXPECT(LoadConfiguredPluginBackends(config, loader).empty());
}

AISTUDIO_TEST(LoadConfiguredPluginBackends_LoadsRealFixtureDll) {
    Config config;
    config.Set("plugin.ids", "fixture");
    config.Set("plugin.fixture.dll", AISTUDIO_PLUGIN_FIXTURE_PATH);
    PluginLoader loader;

    const auto backends = LoadConfiguredPluginBackends(config, loader);

    AISTUDIO_EXPECT(backends.size() == 1);
    AISTUDIO_EXPECT(backends[0]->Id() == "fixture");
    AISTUDIO_EXPECT(loader.IsLoaded("fixture"));
}

AISTUDIO_TEST(LoadConfiguredPluginBackends_IdWithNoDllKey_IsSkipped) {
    Config config;
    config.Set("plugin.ids", "missing");
    // deliberately no plugin.missing.dll
    PluginLoader loader;

    AISTUDIO_EXPECT(LoadConfiguredPluginBackends(config, loader).empty());
    AISTUDIO_EXPECT(!loader.IsLoaded("missing"));
}

AISTUDIO_TEST(LoadConfiguredPluginBackends_NonexistentDllPath_IsSkipped) {
    Config config;
    config.Set("plugin.ids", "bad");
    config.Set("plugin.bad.dll", "this_dll_does_not_exist_aistudio_test.dll");
    PluginLoader loader;

    AISTUDIO_EXPECT(LoadConfiguredPluginBackends(config, loader).empty());
    AISTUDIO_EXPECT(!loader.IsLoaded("bad"));
}

AISTUDIO_TEST(LoadConfiguredPluginBackends_BrokenVTable_IsSkippedAndDllIsUnloaded) {
    Config config;
    config.Set("plugin.ids", "broken");
    config.Set("plugin.broken.dll", AISTUDIO_PLUGIN_FIXTURE_BROKEN_PATH);
    PluginLoader loader;

    AISTUDIO_EXPECT(LoadConfiguredPluginBackends(config, loader).empty());
    // LoadPluginBackend() failed after the DLL itself loaded successfully
    // -- confirms the DLL was unloaded again rather than left dangling.
    AISTUDIO_EXPECT(!loader.IsLoaded("broken"));
}

AISTUDIO_TEST(LoadConfiguredPluginBackends_MixOfValidAndInvalidIds_LoadsOnlyValidOnes) {
    Config config;
    config.Set("plugin.ids", "fixture,missing,bad");
    config.Set("plugin.fixture.dll", AISTUDIO_PLUGIN_FIXTURE_PATH);
    config.Set("plugin.bad.dll", "this_dll_does_not_exist_aistudio_test.dll");
    PluginLoader loader;

    const auto backends = LoadConfiguredPluginBackends(config, loader);

    AISTUDIO_EXPECT(backends.size() == 1);
    AISTUDIO_EXPECT(backends[0]->Id() == "fixture");
}

// ManifestFromBackend() exists so a Backend loaded via
// LoadConfiguredPluginBackends() above can be registered into a
// PluginRegistry (see Core/src/main.cpp's bootstrap) -- without it,
// PluginRegistry::UndeclaredCapabilities() always false-positived on
// every capability a real loaded Plugin advertised, since nothing ever
// told PluginRegistry that Plugin's manifest existed.
AISTUDIO_TEST(ManifestFromBackend_RealFixtureDll_CopiesIdNameVersionCapabilities) {
    Config config;
    config.Set("plugin.ids", "fixture");
    config.Set("plugin.fixture.dll", AISTUDIO_PLUGIN_FIXTURE_PATH);
    PluginLoader loader;
    const auto backends = LoadConfiguredPluginBackends(config, loader);
    AISTUDIO_EXPECT(backends.size() == 1);

    const auto manifest = ManifestFromBackend(*backends[0]);

    AISTUDIO_EXPECT(manifest.id == "fixture");
    AISTUDIO_EXPECT(manifest.name == "Fixture Plugin Backend");
    AISTUDIO_EXPECT(manifest.version == "1.0.0");
    AISTUDIO_EXPECT(manifest.capabilities.size() == 2);
    AISTUDIO_EXPECT(manifest.capabilities[0] == "fixture.ping");
    AISTUDIO_EXPECT(manifest.capabilities[1] == "fixture.echo");
    AISTUDIO_EXPECT(manifest.required_permissions.empty());
}

AISTUDIO_TEST(ManifestFromBackend_RegisteredIntoPluginRegistry_ResolvesUndeclaredCapabilities) {
    Config config;
    config.Set("plugin.ids", "fixture");
    config.Set("plugin.fixture.dll", AISTUDIO_PLUGIN_FIXTURE_PATH);
    PluginLoader loader;
    const auto backends = LoadConfiguredPluginBackends(config, loader);
    AISTUDIO_EXPECT(backends.size() == 1);

    BackendRegistry registry;
    AISTUDIO_EXPECT(registry.Register(backends[0]));

    PluginRegistry plugin_registry;
    // Before registering the manifest: every capability the real Backend
    // advertises is, correctly, still undeclared.
    AISTUDIO_EXPECT(plugin_registry.UndeclaredCapabilities(registry).size() == 2);

    AISTUDIO_EXPECT(plugin_registry.RegisterManifest(ManifestFromBackend(*backends[0])));

    AISTUDIO_EXPECT(plugin_registry.UndeclaredCapabilities(registry).empty());
}
