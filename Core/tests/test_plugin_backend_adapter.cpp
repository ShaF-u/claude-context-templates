#include "test_framework.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Plugin/PluginBackendAdapter.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <any>
#include <string>

using namespace aistudio::core;

// Loads the real aistudio_plugin_fixture/aistudio_plugin_fixture_broken
// DLLs (Core/tests/fixtures/plugin_backend_fixture, built as separate
// CMake targets and located via compile definitions Core/tests/
// CMakeLists.txt sets from $<TARGET_FILE:...>) through PluginLoader ->
// LoadPluginBackend() -> PluginBackendAdapter -- a real cross-DLL ABI
// round trip using Core/Plugin/AbiV1.h, not an in-process mock vtable.
// This is deliberately the more expensive kind of test (a second CMake
// target, real LoadLibraryW calls) because the entire point of this
// feature is that the ABI boundary itself works; a fake in-memory vtable
// would prove the adapter's C++ logic compiles but never prove the actual
// DLL boundary is safe.

namespace {

using Json = nlohmann::json;

std::wstring Widen(const std::string& narrow) {
    // Test-only, ASCII-range build paths only (see PluginLoader.cpp's own
    // MultiByteToWideChar-based conversion for the general case) -- this
    // repo's build directory never contains non-ASCII characters.
    return std::wstring(narrow.begin(), narrow.end());
}

Result<std::unique_ptr<IBackend>> LoadFixture(PluginLoader& loader, const std::string& id, const char* dll_path) {
    const auto load_result = loader.Load(id, Widen(dll_path));
    if (!load_result) {
        return Result<std::unique_ptr<IBackend>>::Fail(load_result.Err());
    }
    return LoadPluginBackend(loader, id);
}

} // namespace

AISTUDIO_TEST(PluginBackendAdapter_LoadFixture_Succeeds) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
}

AISTUDIO_TEST(PluginBackendAdapter_IdNameVersion_MatchFixture) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();
    AISTUDIO_EXPECT(backend->Id() == "fixture");
    AISTUDIO_EXPECT(backend->Name() == "Fixture Plugin Backend");
    AISTUDIO_EXPECT(backend->Version() == "1.0.0");
}

AISTUDIO_TEST(PluginBackendAdapter_Capabilities_MatchFixture) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto capabilities = result.Value()->Capabilities();
    AISTUDIO_EXPECT(capabilities.size() == 2);
    AISTUDIO_EXPECT(capabilities[0] == "fixture.ping");
    AISTUDIO_EXPECT(capabilities[1] == "fixture.echo");
}

AISTUDIO_TEST(PluginBackendAdapter_Health_IsHealthy) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value()->Health() == BackendHealth::Healthy);
}

AISTUDIO_TEST(PluginBackendAdapter_StartStopConfigure_Succeed) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    AISTUDIO_EXPECT(backend->Start());

    Config config;
    config.Set("greeting", "hello");
    AISTUDIO_EXPECT(backend->Configure(config));

    AISTUDIO_EXPECT(backend->Stop());
}

AISTUDIO_TEST(PluginBackendAdapter_Dispatch_ReturnsParsedJsonEcho) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    Command command;
    command.name = "ping";
    command.request_id = "req-1";
    command.payload = std::string("hello plugin");

    const auto dispatch_result = backend->Dispatch(command);
    AISTUDIO_EXPECT(dispatch_result);

    const auto* json_value = std::any_cast<Json>(&dispatch_result.Value());
    AISTUDIO_EXPECT(json_value != nullptr);
    AISTUDIO_EXPECT(json_value->contains("echo"));
    AISTUDIO_EXPECT((*json_value)["echo"]["name"] == "ping");
    AISTUDIO_EXPECT((*json_value)["echo"]["payload"] == "hello plugin");
}

AISTUDIO_TEST(PluginBackendAdapter_Dispatch_FixtureReportsFailure_ReturnsErr) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    Command command;
    command.name = "fail_please"; // fixture scans for the substring "fail"
    const auto dispatch_result = backend->Dispatch(command);
    AISTUDIO_EXPECT(dispatch_result.IsError());
    AISTUDIO_EXPECT(dispatch_result.Err().message == "fixture reports failure");
}

AISTUDIO_TEST(PluginBackendAdapter_Handle_ReturnsParsedJsonEcho) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    Query query;
    query.name = "status";
    const auto handle_result = backend->Handle(query);
    AISTUDIO_EXPECT(handle_result);
    const auto* json_value = std::any_cast<Json>(&handle_result.Value());
    AISTUDIO_EXPECT(json_value != nullptr);
    AISTUDIO_EXPECT((*json_value)["echo"]["name"] == "status");
}

AISTUDIO_TEST(PluginBackendAdapter_Dispatch_EmitEventTrigger_PublishesEventAcrossTheDllBoundary) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    std::vector<Event> received;
    const auto subscription_id = EventBus::Instance().SubscribeEvent(
        "FixtureEvent", [&](const Event& event) { received.push_back(event); });

    Command command;
    command.name = "please emit_event now";
    const auto dispatch_result = backend->Dispatch(command);

    EventBus::Instance().Unsubscribe("FixtureEvent", subscription_id);

    AISTUDIO_EXPECT(dispatch_result); // the trigger command still dispatches normally too
    AISTUDIO_EXPECT(received.size() == 1);
    AISTUDIO_EXPECT(received[0].source == "fixture"); // PluginBackendAdapter::Id(), not something the fixture set itself
    AISTUDIO_EXPECT(received[0].name == "FixtureEvent");
    const auto* payload = std::any_cast<Json>(&received[0].payload);
    AISTUDIO_EXPECT(payload != nullptr);
    AISTUDIO_EXPECT((*payload)["triggered"] == true);
}

AISTUDIO_TEST(PluginBackendAdapter_Dispatch_WithoutEmitEventTrigger_PublishesNoEvent) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    int received_count = 0;
    const auto subscription_id =
        EventBus::Instance().SubscribeEvent("FixtureEvent", [&](const Event&) { ++received_count; });

    Command command;
    command.name = "ordinary command";
    backend->Dispatch(command);

    EventBus::Instance().Unsubscribe("FixtureEvent", subscription_id);
    AISTUDIO_EXPECT(received_count == 0);
}

AISTUDIO_TEST(PluginBackendAdapter_ProvideContext_ReturnsRealItemFromFixture) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    const auto items = backend->ProvideContext("looking for fixture_context please");
    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].id == "fixture:item1");
    AISTUDIO_EXPECT(items[0].content == "fixture context item");
    AISTUDIO_EXPECT(items[0].priority == 42);
    AISTUDIO_EXPECT(items[0].source == ContextSourceKind::Custom);
    AISTUDIO_EXPECT(items[0].estimated_tokens == EstimateTokens(items[0].content)); // recomputed, not Plugin-supplied
}

AISTUDIO_TEST(PluginBackendAdapter_ProvideContext_NoMatchingTrigger_ReturnsEmpty) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value()->ProvideContext("nothing relevant here").empty());
}

AISTUDIO_TEST(PluginBackendAdapter_RenderUiDescription_ReturnsFixturePanel) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    const auto panel = backend->RenderUiDescription();
    AISTUDIO_EXPECT(panel.title == "Fixture Panel");
    AISTUDIO_EXPECT(panel.elements.size() == 3);
    AISTUDIO_EXPECT(panel.elements[0].kind == UiElementKind::Text);
    AISTUDIO_EXPECT(panel.elements[0].text == "hello from fixture");
    AISTUDIO_EXPECT(panel.elements[1].kind == UiElementKind::Separator);
    AISTUDIO_EXPECT(panel.elements[2].kind == UiElementKind::Button);
    AISTUDIO_EXPECT(panel.elements[2].text == "Ping");
    AISTUDIO_EXPECT(panel.elements[2].action_id == "fixture_button_clicked");
}

AISTUDIO_TEST(PluginBackendAdapter_RenderUiDescription_ButtonActionId_DispatchesLikeAnyOtherCommand) {
    PluginLoader loader;
    const auto result = LoadFixture(loader, "fixture", AISTUDIO_PLUGIN_FIXTURE_PATH);
    AISTUDIO_EXPECT(result);
    const auto& backend = result.Value();

    const auto panel = backend->RenderUiDescription();
    const auto& button = panel.elements[2];

    Command command;
    command.name = button.action_id;
    const auto dispatch_result = backend->Dispatch(command);
    AISTUDIO_EXPECT(dispatch_result); // reuses the fixture's normal echo behavior, nothing UI-specific
    const auto* json_value = std::any_cast<Json>(&dispatch_result.Value());
    AISTUDIO_EXPECT(json_value != nullptr);
    AISTUDIO_EXPECT((*json_value)["echo"]["name"] == "fixture_button_clicked");
}

AISTUDIO_TEST(LoadPluginBackend_NotLoaded_Fails) {
    PluginLoader loader;
    const auto result = LoadPluginBackend(loader, "not-loaded");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(LoadPluginBackend_BrokenVTable_NullFunctionPointer_Fails) {
    PluginLoader loader;
    const auto load_result = loader.Load("broken", Widen(AISTUDIO_PLUGIN_FIXTURE_BROKEN_PATH));
    AISTUDIO_EXPECT(load_result);

    const auto result = LoadPluginBackend(loader, "broken");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(LoadPluginBackend_DoesNotExportEntry_Fails) {
    // kernel32.dll is a real, always-present DLL (same choice
    // test_plugin_loader.cpp makes) that certainly doesn't export
    // AistudioPluginEntry.
    PluginLoader loader;
    AISTUDIO_EXPECT(loader.Load("k32", L"kernel32.dll"));
    const auto result = LoadPluginBackend(loader, "k32");
    AISTUDIO_EXPECT(result.IsError());
}
