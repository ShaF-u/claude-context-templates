#include "Core/Plugin/PluginBootstrap.hpp"

#include "Core/Database/StringList.hpp"
#include "Core/Logging/Logger.hpp"
#include "Core/Plugin/PluginBackendAdapter.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aistudio::core {

namespace {

std::wstring WidenUtf8(const std::string& utf8) {
#if defined(_WIN32)
    if (utf8.empty()) {
        return std::wstring();
    }
    const int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wide_length);
    return wide;
#else
    // PluginLoader::Load() itself already no-ops/fails on this platform
    // regardless of what path it's given (see PluginLoader.cpp) -- this
    // ASCII-only fallback exists only so the code compiles everywhere.
    return std::wstring(utf8.begin(), utf8.end());
#endif
}

} // namespace

std::vector<std::shared_ptr<IBackend>> LoadConfiguredPluginBackends(const Config& config, PluginLoader& loader) {
    std::vector<std::shared_ptr<IBackend>> backends;

    for (const auto& id : SplitStringList(config.GetOr("plugin.ids", ""))) {
        const auto dll_path = config.GetOr("plugin." + id + ".dll", "");
        if (dll_path.empty()) {
            AISTUDIO_LOG_WARN("Core.Plugin.Bootstrap", "plugin '" + id + "' listed in plugin.ids but has no plugin." +
                                                             id + ".dll -- skipped");
            continue;
        }

        if (const auto load_result = loader.Load(id, WidenUtf8(dll_path)); !load_result) {
            AISTUDIO_LOG_WARN("Core.Plugin.Bootstrap",
                               "failed to load plugin '" + id + "': " + load_result.Err().message);
            continue;
        }

        auto backend_result = LoadPluginBackend(loader, id);
        if (!backend_result) {
            AISTUDIO_LOG_WARN("Core.Plugin.Bootstrap",
                               "failed to initialize plugin '" + id + "': " + backend_result.Err().message);
            loader.Unload(id); // don't leave a DLL loaded with nothing registered for it
            continue;
        }

        AISTUDIO_LOG_INFO("Core.Plugin.Bootstrap", "loaded plugin '" + id + "' from " + dll_path);
        backends.push_back(std::shared_ptr<IBackend>(std::move(backend_result.Value())));
    }

    return backends;
}

PluginManifest ManifestFromBackend(const IBackend& backend) {
    PluginManifest manifest;
    manifest.id = backend.Id();
    manifest.name = backend.Name();
    manifest.version = backend.Version();
    manifest.capabilities = backend.Capabilities();
    return manifest;
}

} // namespace aistudio::core
