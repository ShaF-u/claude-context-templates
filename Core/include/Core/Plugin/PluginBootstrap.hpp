#pragma once

#include "Core/Backend/IBackend.hpp"
#include "Core/Config/Config.hpp"
#include "Core/Plugin/PluginLoader.hpp"
#include "Core/Plugin/PluginManifest.hpp"

#include <memory>
#include <vector>

namespace aistudio::core {

// Loads every Plugin Backend declared in `config` (docs/ROADMAP.md Phase 11
// "Plugin SDK" -- the bootstrap-facing counterpart of LoadPluginBackend()/
// PluginLoader, which by themselves require a caller to explicitly invoke
// them; this is that caller, but still only when the config asks for it).
//
// Reads `plugin.ids` (comma-joined, Core/Database/StringList.hpp's
// SplitStringList()/JoinStringList() format -- ids in this codebase don't
// contain commas) for which plugin ids to load, and `plugin.<id>.dll` for
// each one's DLL path. Entirely opt-in: an empty/absent `plugin.ids`
// loads nothing, and an id with no matching `plugin.<id>.dll` key is
// skipped (logged, not fatal). This is the ONLY place a Plugin DLL gets
// loaded automatically anywhere in this Studio.
//
// No Sandbox check is applied to the DLL path -- Sandbox governs file
// *content* access, never native code execution (PluginLoader.hpp's own
// class comment), so a path check here would be misleading security
// theater rather than real protection. This Studio ships as a single
// local .exe the user runs on their own machine (CLAUDE.md's 2026-09-01
// GUI/distribution policy), not a remote/multi-tenant service -- a DLL
// path the user wrote into their own aistudio.config carries exactly the
// same trust as any other program they choose to run.
//
// `loader` must outlive every returned IBackend (LoadPluginBackend()'s
// own existing requirement) -- pass the same long-lived PluginLoader the
// caller keeps for the rest of the app's lifetime. A plugin id that
// fails at any step (DLL missing, ABI mismatch, malformed vtable) is
// logged and skipped rather than aborting the remaining ids; its DLL (if
// it got that far) is unloaded again rather than left loaded unused.
[[nodiscard]] std::vector<std::shared_ptr<IBackend>> LoadConfiguredPluginBackends(const Config& config,
                                                                                   PluginLoader& loader);

// Builds a PluginManifest describing `backend`'s Id()/Name()/Version()/
// Capabilities() -- for registering a Backend returned by
// LoadConfiguredPluginBackends() above into a PluginRegistry, so
// PluginRegistry::UndeclaredCapabilities() doesn't false-positive on a
// real loaded Plugin whose capabilities it never otherwise learns about
// (LoadConfiguredPluginBackends() itself only deals in IBackend, never
// PluginManifest). `required_permissions` is always left empty: AbiV1.h's
// vtable has no channel for a Plugin to declare these yet, so there is
// nothing here to synthesize it from -- a known gap (docs/ROADMAP.md
// Phase 11), not an oversight.
[[nodiscard]] PluginManifest ManifestFromBackend(const IBackend& backend);

} // namespace aistudio::core
