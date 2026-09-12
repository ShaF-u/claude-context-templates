#pragma once

#include "Core/Backend/UiDescription.hpp"
#include "Core/Config/Config.hpp"
#include "Core/Context/ContextItem.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Protocol/Protocol.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

enum class BackendHealth { Unknown, Healthy, Degraded, Unavailable };

[[nodiscard]] std::string ToString(BackendHealth health);

// A Backend's formal lifecycle — tracked by BackendRegistry, driven by
// its StartBackend()/StopBackend()/StartAll()/StopAll() (docs/ROADMAP.md
// "Backend Lifecycle の正式な状態機械"). A freshly Register()'d Backend
// starts at Registered, having never had Start() called. Failed means
// Start() or Stop() itself returned an error — a Backend can be
// restarted from Failed (Start() is tried again) the same as from
// Stopped.
enum class BackendLifecycleState { Registered, Starting, Running, Stopping, Stopped, Failed };

[[nodiscard]] std::string ToString(BackendLifecycleState state);

// Common surface every specialized Backend (Unreal, Unity, Git, IDE, ...)
// implements. AI Development Studio talks to Backends only through this
// interface — Command to act, Query to read — never through
// Backend-specific internals (AGENT.md #2, docs/MASTER_SPEC.md #31-34).
class IBackend {
public:
    virtual ~IBackend() = default;

    [[nodiscard]] virtual std::string Id() const = 0;
    [[nodiscard]] virtual std::string Name() const = 0;
    [[nodiscard]] virtual std::string Version() const = 0;
    [[nodiscard]] virtual std::vector<std::string> Capabilities() const = 0;

    [[nodiscard]] virtual BackendHealth Health() const = 0;

    virtual Result<void> Start() = 0;
    virtual Result<void> Stop() = 0;

    // Called once after registration with whatever Config the caller has
    // available (conventionally keys under `backend.<Id()>.*`). Default
    // no-op — override only if the Backend actually needs configuration
    // (docs/MASTER_SPEC.md #31 Backend Configuration).
    virtual Result<void> Configure(const Config& config) {
        (void)config;
        return Result<void>::Ok();
    }

    // Command.backend_id / Query.backend_id are expected to already match
    // Id() — BackendRegistry::Dispatch/Query take care of routing, these
    // are the per-Backend handlers.
    virtual CommandResult Dispatch(const Command& command) = 0;
    virtual QueryResult Handle(const Query& query) = 0;

    // Context items this Backend can contribute for a free-text intent
    // (docs/MASTER_SPEC.md #71 Context Provider System) -- e.g. a future
    // native GitBackend returning recent commits mentioning `intent`, or
    // a Plugin Backend (PluginBackendAdapter) forwarding through its own
    // optional provide_context ABI function. Default: contributes
    // nothing -- override only if the Backend actually has context to
    // contribute (mirrors Configure()'s own opt-in default just above).
    // ContextRetriever (when given a BackendRegistry) calls this
    // alongside its own Symbol/File/Dependency/Keyword retrieval and
    // merges the results, still subject to whatever firewall it was
    // constructed with.
    [[nodiscard]] virtual std::vector<ContextItem> ProvideContext(const std::string& intent) const {
        (void)intent;
        return {};
    }

    // This Backend's optional GUI panel (docs/ROADMAP.md Phase 11 "UI
    // extensions", docs/MASTER_SPEC.md's UI Extension System) -- see
    // Core/Backend/UiDescription.hpp for the vocabulary. Default:
    // nothing to render, same opt-in pattern as Configure()/
    // ProvideContext() just above. The GUI (a separate process from
    // aistudio_core_cli that also links aistudio_core) calls this on
    // demand (its own "refresh" action, not every frame) for every
    // registered Backend and renders whatever comes back.
    [[nodiscard]] virtual UiPanelDescription RenderUiDescription() const { return {}; }
};

} // namespace aistudio::core
