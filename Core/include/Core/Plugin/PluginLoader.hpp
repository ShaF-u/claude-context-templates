#pragma once

#include "Core/Error/Result.hpp"

#include <string>
#include <unordered_map>

namespace aistudio::core {

// Loads/unloads a Plugin's native code by explicit id, and resolves a
// named export from it -- the load-mechanics layer underneath Phase 11's
// still-undecided Plugin SDK question of how a Plugin's code actually
// runs (docs/ROADMAP.md Phase 11 "Plugin SDK"). Scope for this first
// pass (AGENT.md #14 "最小実装優先"): in-process DLL loading only,
// matching common native desktop-plugin precedent (VST/DAW-style
// plugins) rather than an out-of-process/IPC design -- simpler, and
// consistent with this Studio's lightweight/low-resource design
// philosophy (CLAUDE.md), at the real cost of running loaded code with
// the same privileges and address space as the host process: there is
// NO sandboxing here, and none is implied by Sandbox/PermissionPolicy
// (those govern file paths and Command dispatch, not arbitrary native
// code execution) -- a loaded Plugin can do anything this process can.
//
// This class does ONLY the load/resolve/unload mechanics. It does not by
// itself define what a Plugin DLL is expected to export -- that ABI
// contract now exists (Core/Plugin/AbiV1.h, resolved via
// LoadPluginBackend() in Core/Plugin/PluginBackendAdapter.hpp) as a
// separate layer built on top of this class, not inside it -- and nothing
// in this Studio calls either of them automatically yet: no startup/
// bootstrap path loads any DLL on its own. A caller must explicitly
// construct a PluginLoader and call Load().
//
// Windows-only for now (LoadLibraryW/GetProcAddress/FreeLibrary) -- no
// other platform's dynamic loading is implemented anywhere else in this
// Studio (CLAUDE.md's shipped target is a single native Windows .exe),
// so a POSIX dlopen() fallback would be speculative rather than needed.
class PluginLoader {
public:
    PluginLoader() = default;
    ~PluginLoader(); // unloads every library still loaded

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // Loads `library_path` (a .dll) and tracks it under `id`. Fails if
    // `id` is already loaded (Unload() it first to reload) or the OS load
    // itself fails (missing file, wrong architecture, an unresolved
    // dependency DLL, etc. -- the failure message includes the OS error).
    Result<void> Load(const std::string& id, const std::wstring& library_path);

    // No-op if `id` isn't currently loaded.
    void Unload(const std::string& id);

    [[nodiscard]] bool IsLoaded(const std::string& id) const;

    // Resolves `symbol_name` within the library loaded under `id`.
    // nullptr if `id` isn't loaded or doesn't export that symbol.
    [[nodiscard]] void* ResolveSymbol(const std::string& id, const std::string& symbol_name) const;

private:
    // Holds an HMODULE per id -- kept as void* so this header doesn't
    // need to pull in <windows.h> (matches the Core/Gui split: Windows
    // headers stay out of Core's public interface where avoidable).
    std::unordered_map<std::string, void*> handles_;
};

} // namespace aistudio::core
