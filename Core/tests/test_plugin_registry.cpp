#include "test_framework.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Plugin/PluginRegistry.hpp"

#include <algorithm>
#include <any>
#include <vector>

using namespace aistudio::core;

namespace {

class FakePluginBackend final : public IBackend {
public:
    explicit FakePluginBackend(std::vector<std::string> capabilities) : capabilities_(std::move(capabilities)) {}

    [[nodiscard]] std::string Id() const override { return "fake-plugin-backend"; }
    [[nodiscard]] std::string Name() const override { return "Fake Plugin Backend"; }
    [[nodiscard]] std::string Version() const override { return "0.0.1"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override { return capabilities_; }
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }
    CommandResult Dispatch(const Command&) override {
        return CommandResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }
    QueryResult Handle(const Query&) override {
        return QueryResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }

private:
    std::vector<std::string> capabilities_;
};

PluginManifest MakeManifest(std::string id, std::vector<std::string> capabilities = {}) {
    PluginManifest manifest;
    manifest.id = std::move(id);
    manifest.name = manifest.id;
    manifest.version = "0.0.1";
    manifest.capabilities = std::move(capabilities);
    return manifest;
}

} // namespace

AISTUDIO_TEST(PluginRegistry_AllRequiredPermissions_EmptyRegistry_ReturnsEmpty) {
    PluginRegistry registry;
    AISTUDIO_EXPECT(registry.AllRequiredPermissions().empty());
}

AISTUDIO_TEST(PluginRegistry_AllRequiredPermissions_CollectsFromEveryManifest) {
    PluginRegistry registry;
    auto a = MakeManifest("plugin.a");
    a.required_permissions = {"plugin.a.*"};
    registry.RegisterManifest(a);
    auto b = MakeManifest("plugin.b");
    b.required_permissions = {"plugin.b.delete"};
    registry.RegisterManifest(b);

    const auto patterns = registry.AllRequiredPermissions();
    AISTUDIO_EXPECT(patterns.size() == 2);
    AISTUDIO_EXPECT(std::find(patterns.begin(), patterns.end(), "plugin.a.*") != patterns.end());
    AISTUDIO_EXPECT(std::find(patterns.begin(), patterns.end(), "plugin.b.delete") != patterns.end());
}

AISTUDIO_TEST(PluginRegistry_AllRequiredPermissions_DeduplicatesSharedPatterns) {
    PluginRegistry registry;
    auto a = MakeManifest("plugin.a");
    a.required_permissions = {"shared.pattern"};
    registry.RegisterManifest(a);
    auto b = MakeManifest("plugin.b");
    b.required_permissions = {"shared.pattern"};
    registry.RegisterManifest(b);

    AISTUDIO_EXPECT(registry.AllRequiredPermissions().size() == 1);
}

AISTUDIO_TEST(PluginRegistry_RegisterAndFind) {
    PluginRegistry registry;
    AISTUDIO_EXPECT(registry.RegisterManifest(MakeManifest("core.null")));
    AISTUDIO_EXPECT(registry.Find("core.null").has_value());
}

AISTUDIO_TEST(PluginRegistry_Register_EmptyId_Fails) {
    PluginRegistry registry;
    AISTUDIO_EXPECT(registry.RegisterManifest(MakeManifest("")).IsError());
}

AISTUDIO_TEST(PluginRegistry_Register_DuplicateId_Fails) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    AISTUDIO_EXPECT(registry.RegisterManifest(MakeManifest("core.null")).IsError());
}

AISTUDIO_TEST(PluginRegistry_Unregister_RemovesManifest) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    registry.UnregisterManifest("core.null");
    AISTUDIO_EXPECT(!registry.Find("core.null").has_value());
}

AISTUDIO_TEST(PluginRegistry_All_ListsEveryManifest) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("a"));
    registry.RegisterManifest(MakeManifest("b"));
    AISTUDIO_EXPECT(registry.All().size() == 2);
}

AISTUDIO_TEST(PluginRegistry_UndeclaredCapabilities_EmptyWhenFullyDeclared) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null", {"diagnostics.ping"}));

    BackendRegistry backend_registry;
    backend_registry.Register(std::make_shared<FakePluginBackend>(std::vector<std::string>{"diagnostics.ping"}));

    AISTUDIO_EXPECT(registry.UndeclaredCapabilities(backend_registry).empty());
}

AISTUDIO_TEST(PluginRegistry_UndeclaredCapabilities_ReportsMissingDeclaration) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null", {"diagnostics.ping"}));

    BackendRegistry backend_registry;
    backend_registry.Register(
        std::make_shared<FakePluginBackend>(std::vector<std::string>{"diagnostics.ping", "diagnostics.delete"}));

    const auto undeclared = registry.UndeclaredCapabilities(backend_registry);
    AISTUDIO_EXPECT(undeclared.size() == 1);
    AISTUDIO_EXPECT(undeclared.front() == "diagnostics.delete");
}

AISTUDIO_TEST(PluginRegistry_UndeclaredCapabilities_NoManifests_ReportsAll) {
    PluginRegistry registry;

    BackendRegistry backend_registry;
    backend_registry.Register(std::make_shared<FakePluginBackend>(std::vector<std::string>{"diagnostics.ping"}));

    const auto undeclared = registry.UndeclaredCapabilities(backend_registry);
    AISTUDIO_EXPECT(undeclared.size() == 1);
}

AISTUDIO_TEST(PluginRegistry_RegisterManifest_SetsInitialLifecycleStateToRegistered) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    AISTUDIO_EXPECT(registry.LifecycleState("core.null") == PluginLifecycleState::Registered);
}

AISTUDIO_TEST(PluginRegistry_LifecycleState_UnknownId_ReturnsNullopt) {
    PluginRegistry registry;
    AISTUDIO_EXPECT(registry.LifecycleState("does-not-exist") == std::nullopt);
}

AISTUDIO_TEST(PluginRegistry_UnregisterManifest_ClearsLifecycleState) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    registry.UnregisterManifest("core.null");
    AISTUDIO_EXPECT(registry.LifecycleState("core.null") == std::nullopt);
}

AISTUDIO_TEST(PluginRegistry_EnablePlugin_TransitionsToEnabled) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    AISTUDIO_EXPECT(registry.EnablePlugin("core.null"));
    AISTUDIO_EXPECT(registry.LifecycleState("core.null") == PluginLifecycleState::Enabled);
}

AISTUDIO_TEST(PluginRegistry_EnablePlugin_UnknownId_Fails) {
    PluginRegistry registry;
    AISTUDIO_EXPECT(registry.EnablePlugin("does-not-exist").IsError());
}

AISTUDIO_TEST(PluginRegistry_DisablePlugin_TransitionsToDisabled) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    registry.EnablePlugin("core.null");
    AISTUDIO_EXPECT(registry.DisablePlugin("core.null"));
    AISTUDIO_EXPECT(registry.LifecycleState("core.null") == PluginLifecycleState::Disabled);
}

AISTUDIO_TEST(PluginRegistry_DisablePlugin_UnknownId_Fails) {
    PluginRegistry registry;
    AISTUDIO_EXPECT(registry.DisablePlugin("does-not-exist").IsError());
}

AISTUDIO_TEST(PluginRegistry_EnablePlugin_CanReEnableAfterDisable) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    registry.EnablePlugin("core.null");
    registry.DisablePlugin("core.null");
    AISTUDIO_EXPECT(registry.EnablePlugin("core.null"));
    AISTUDIO_EXPECT(registry.LifecycleState("core.null") == PluginLifecycleState::Enabled);
}

AISTUDIO_TEST(PluginRegistry_EnablePlugin_PublishesLifecycleChangedEvent) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));

    std::vector<PluginLifecycleChange> received;
    const auto subscription_id =
        EventBus::Instance().Subscribe("PluginLifecycleChanged", [&](const std::any& payload) {
            if (const auto* change = std::any_cast<PluginLifecycleChange>(&payload)) {
                received.push_back(*change);
            }
        });

    registry.EnablePlugin("core.null");

    EventBus::Instance().Unsubscribe("PluginLifecycleChanged", subscription_id);
    AISTUDIO_EXPECT(received.size() == 1);
    AISTUDIO_EXPECT(received[0].plugin_id == "core.null");
    AISTUDIO_EXPECT(received[0].previous == PluginLifecycleState::Registered);
    AISTUDIO_EXPECT(received[0].current == PluginLifecycleState::Enabled);
}

AISTUDIO_TEST(PluginRegistry_EnablePlugin_AlreadyEnabled_IsNoOpAndPublishesNoEvent) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeManifest("core.null"));
    registry.EnablePlugin("core.null");

    int event_count = 0;
    const auto subscription_id = EventBus::Instance().Subscribe(
        "PluginLifecycleChanged", [&](const std::any&) { ++event_count; });

    AISTUDIO_EXPECT(registry.EnablePlugin("core.null"));

    EventBus::Instance().Unsubscribe("PluginLifecycleChanged", subscription_id);
    AISTUDIO_EXPECT(event_count == 0);
    AISTUDIO_EXPECT(registry.LifecycleState("core.null") == PluginLifecycleState::Enabled);
}

AISTUDIO_TEST(ToString_PluginLifecycleState_CoversAllValues) {
    AISTUDIO_EXPECT(ToString(PluginLifecycleState::Registered) == "Registered");
    AISTUDIO_EXPECT(ToString(PluginLifecycleState::Enabled) == "Enabled");
    AISTUDIO_EXPECT(ToString(PluginLifecycleState::Disabled) == "Disabled");
}

PluginManifest MakeVersionedManifest(std::string id, std::string version) {
    PluginManifest manifest = MakeManifest(std::move(id));
    manifest.version = std::move(version);
    return manifest;
}

AISTUDIO_TEST(PluginRegistry_FindCompatible_NoVersionRequired_MatchesAnyManifest) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeVersionedManifest("core.null", "2.1.0"));
    AISTUDIO_EXPECT(registry.FindCompatible("core.null", std::nullopt).has_value());
}

AISTUDIO_TEST(PluginRegistry_FindCompatible_CompatibleVersionRequired_Matches) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeVersionedManifest("core.null", "2.1.0"));
    AISTUDIO_EXPECT(registry.FindCompatible("core.null", CapabilityVersion{2, 0, 0}).has_value());
}

AISTUDIO_TEST(PluginRegistry_FindCompatible_IncompatibleMajorVersionRequired_NoMatch) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeVersionedManifest("core.null", "2.1.0"));
    AISTUDIO_EXPECT(!registry.FindCompatible("core.null", CapabilityVersion{3, 0, 0}).has_value());
}

AISTUDIO_TEST(PluginRegistry_FindCompatible_NewerMinorRequiredThanProvided_NoMatch) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeVersionedManifest("core.null", "2.1.0"));
    AISTUDIO_EXPECT(!registry.FindCompatible("core.null", CapabilityVersion{2, 9, 0}).has_value());
}

AISTUDIO_TEST(PluginRegistry_FindCompatible_VersionRequiredForUnparseableVersion_NoMatch) {
    PluginRegistry registry;
    registry.RegisterManifest(MakeVersionedManifest("core.null", "not-a-version"));
    AISTUDIO_EXPECT(!registry.FindCompatible("core.null", CapabilityVersion{1, 0, 0}).has_value());
}

AISTUDIO_TEST(PluginRegistry_FindCompatible_UnknownId_ReturnsNullopt) {
    PluginRegistry registry;
    AISTUDIO_EXPECT(!registry.FindCompatible("does-not-exist", std::nullopt).has_value());
}
