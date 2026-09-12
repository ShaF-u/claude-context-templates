#include "Core/Plugin/PluginLoader.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aistudio::core {

#if defined(_WIN32)

namespace {

std::string FormatLastError(DWORD error_code) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "error code " + std::to_string(error_code);
    }
    // Narrow via WideCharToMultiByte rather than std::wstring_convert
    // (deprecated in C++17) -- same conversion need as elsewhere in this
    // codebase's Windows-specific paths.
    const int utf8_length =
        ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string message(static_cast<size_t>(utf8_length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), message.data(), utf8_length, nullptr,
                          nullptr);
    ::LocalFree(buffer);
    // FormatMessageW's text ends with "\r\n"; trim it for a cleaner Error.message.
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

} // namespace

PluginLoader::~PluginLoader() {
    for (const auto& [id, handle] : handles_) {
        ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
    }
}

Result<void> PluginLoader::Load(const std::string& id, const std::wstring& library_path) {
    if (handles_.contains(id)) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "plugin already loaded: " + id,
            .module = "Core.Plugin.Loader",
        });
    }

    const HMODULE handle = ::LoadLibraryW(library_path.c_str());
    if (handle == nullptr) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "LoadLibraryW failed for '" + id + "': " + FormatLastError(::GetLastError()),
            .module = "Core.Plugin.Loader",
        });
    }

    handles_.emplace(id, reinterpret_cast<void*>(handle));
    return Result<void>::Ok();
}

void PluginLoader::Unload(const std::string& id) {
    const auto it = handles_.find(id);
    if (it == handles_.end()) {
        return;
    }
    ::FreeLibrary(reinterpret_cast<HMODULE>(it->second));
    handles_.erase(it);
}

bool PluginLoader::IsLoaded(const std::string& id) const {
    return handles_.contains(id);
}

void* PluginLoader::ResolveSymbol(const std::string& id, const std::string& symbol_name) const {
    const auto it = handles_.find(id);
    if (it == handles_.end()) {
        return nullptr;
    }
    return reinterpret_cast<void*>(::GetProcAddress(reinterpret_cast<HMODULE>(it->second), symbol_name.c_str()));
}

#else

// Non-Windows: no dynamic loading backend exists yet (see PluginLoader.hpp's
// class comment) -- every operation fails/no-ops rather than leaving these
// symbols undefined, so a non-Windows build of Core still links.
PluginLoader::~PluginLoader() = default;

Result<void> PluginLoader::Load(const std::string& id, const std::wstring&) {
    return Result<void>::Fail(Error{
        .code = ErrorCode::Internal,
        .message = "PluginLoader is not implemented on this platform: " + id,
        .module = "Core.Plugin.Loader",
    });
}

void PluginLoader::Unload(const std::string&) {}

bool PluginLoader::IsLoaded(const std::string&) const {
    return false;
}

void* PluginLoader::ResolveSymbol(const std::string&, const std::string&) const {
    return nullptr;
}

#endif

} // namespace aistudio::core
