#pragma once

#include "Core/Backend/IBackend.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Plugin/AbiV1.h"
#include "Core/Plugin/PluginLoader.hpp"

#include <memory>
#include <string>

namespace aistudio::core {

// Forward-declared with C linkage here so PluginBackendAdapter below can
// befriend this exact declaration -- defined in PluginBackendAdapter.cpp,
// it's the actual function pointer value stored in
// AistudioPluginHostServices::publish_event (see AbiV1.h), i.e. the one
// call flowing from a Plugin DLL back into Core rather than the reverse.
extern "C" void PluginHostPublishEventTrampoline(void* host_context, AistudioPluginString event_name,
                                                  AistudioPluginString payload_json);

// Wraps a Plugin's C-ABI AistudioPluginBackendVTable (AbiV1.h) as a
// regular IBackend -- the bridge that lets BackendRegistry/ApiServer/
// PermissionPolicy/Sandbox all keep treating a Plugin Backend exactly
// like NullBackend or any other in-process IBackend, never aware that
// calls are actually crossing a DLL boundary underneath. Owns the
// AistudioPluginBackend* instance for its own lifetime: create() in the
// constructor, destroy() in the destructor.
//
// Every AistudioPluginString the vtable returns is copied into a real
// std::string immediately (see AbiV1.h's borrowed-view lifetime rule) --
// nothing here retains a pointer into Plugin memory past the call that
// produced it.
//
// If the vtable provides bind_host (optional, see AbiV1.h), the
// constructor calls it once with an AistudioPluginHostServices whose
// host_context is `this` -- letting the Plugin later call publish_event()
// (PluginHostPublishEventTrampoline above, which casts host_context back
// to a PluginBackendAdapter*) to publish an Event onto EventBus,
// attributed to this adapter's own Id() via Event::source.
class PluginBackendAdapter final : public IBackend {
public:
    // `vtable` must outlive this adapter (it points into the loaded DLL's
    // own static data, which PluginLoader keeps loaded for as long as the
    // caller holds the PluginLoader alive -- see LoadPluginBackend below
    // for the usual construction path, which enforces this by
    // construction rather than leaving it to the caller to remember).
    explicit PluginBackendAdapter(const AistudioPluginBackendVTable* vtable);
    ~PluginBackendAdapter() override;

    PluginBackendAdapter(const PluginBackendAdapter&) = delete;
    PluginBackendAdapter& operator=(const PluginBackendAdapter&) = delete;

    [[nodiscard]] std::string Id() const override;
    [[nodiscard]] std::string Name() const override;
    [[nodiscard]] std::string Version() const override;
    [[nodiscard]] std::vector<std::string> Capabilities() const override;

    [[nodiscard]] BackendHealth Health() const override;

    Result<void> Start() override;
    Result<void> Stop() override;
    Result<void> Configure(const Config& config) override;

    CommandResult Dispatch(const Command& command) override;
    QueryResult Handle(const Query& query) override;

    // Empty if the vtable's provide_context is NULL (optional, see
    // AbiV1.h) or the Plugin returns malformed JSON -- logged, not
    // thrown, matching Dispatch()/Handle()'s own tolerance for a
    // misbehaving Plugin over crashing the caller.
    [[nodiscard]] std::vector<ContextItem> ProvideContext(const std::string& intent) const override;

    // Default-constructed (no title, no elements -- "nothing to
    // render") if the vtable's render_ui is NULL (optional, see AbiV1.h)
    // or the Plugin returns malformed JSON, same tolerance as
    // ProvideContext() above.
    [[nodiscard]] UiPanelDescription RenderUiDescription() const override;

private:
    // Only PluginHostPublishEventTrampoline (the actual
    // AistudioPluginHostServices::publish_event function pointer a
    // Plugin calls, forward-declared above) may reach PublishEvent() -- a
    // friend function rather than a public method since it isn't part of
    // IBackend or otherwise meant for a normal C++ caller to reach
    // directly.
    friend void PluginHostPublishEventTrampoline(void* host_context, AistudioPluginString event_name,
                                                  AistudioPluginString payload_json);

    // Handles a publish_event() call from the Plugin -- parses
    // `payload_json` (skipping the parse entirely for a zero-length
    // view, logging and skipping the publish for malformed JSON) and
    // calls EventBus::Instance().Publish() with Event::source set to
    // Id().
    void PublishEvent(AistudioPluginString event_name, AistudioPluginString payload_json);

    const AistudioPluginBackendVTable* vtable_;
    AistudioPluginBackend* instance_;
    AistudioPluginHostServices host_services_{};
};

// Resolves `id`'s AistudioPluginEntry export (the Plugin must already be
// loaded via `loader.Load(id, ...)`), negotiates AISTUDIO_PLUGIN_ABI_VERSION,
// validates the returned vtable has every required function pointer set,
// and wraps it in a PluginBackendAdapter ready to hand to
// BackendRegistry::Register(). Fails (rather than crash later) if: `id`
// isn't loaded, the DLL doesn't export AistudioPluginEntry, the Plugin
// rejects the host's ABI version (or vice versa), or any vtable function
// pointer is NULL.
//
// `loader` must outlive the returned IBackend -- unloading the Plugin
// while its PluginBackendAdapter is still registered would leave that
// adapter's vtable pointer dangling into unmapped memory (PluginLoader
// itself does not track or prevent this; the same caution applies as any
// other Load()/Unload() pairing).
[[nodiscard]] Result<std::unique_ptr<IBackend>> LoadPluginBackend(const PluginLoader& loader, const std::string& id);

} // namespace aistudio::core
