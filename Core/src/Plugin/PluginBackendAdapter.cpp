#include "Core/Plugin/PluginBackendAdapter.hpp"

#include "Core/Event/EventBus.hpp"
#include "Core/Logging/Logger.hpp"

// Vendored header-only third-party library (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since this header isn't ours to fix --
// same pattern as Core/src/API/ApiServer.cpp and Core/src/MCP/McpServer.cpp.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace aistudio::core {

namespace {

using Json = nlohmann::json;

std::string ToStdString(AistudioPluginString s) {
    return s.data == nullptr ? std::string() : std::string(s.data, s.len);
}

AistudioPluginString ToPluginString(const std::string& s) {
    return AistudioPluginString{s.data(), s.size()};
}

BackendHealth MapHealth(int value) {
    switch (value) {
        case AISTUDIO_PLUGIN_HEALTH_HEALTHY: return BackendHealth::Healthy;
        case AISTUDIO_PLUGIN_HEALTH_DEGRADED: return BackendHealth::Degraded;
        case AISTUDIO_PLUGIN_HEALTH_UNAVAILABLE: return BackendHealth::Unavailable;
        default: return BackendHealth::Unknown;
    }
}

// Command::payload / Query::parameters are only ever forwarded to a
// Plugin as a plain string (empty if absent or not a string) -- the same
// restriction Core/API/ApiServer.cpp's own POST .../commands endpoint
// already places on an incoming Command::payload, kept consistent rather
// than inventing a richer shape only Plugins would see.
std::string AnyToPluginPayloadString(const std::any& value) {
    if (const auto* s = std::any_cast<std::string>(&value)) {
        return *s;
    }
    return "";
}

Result<std::any> ParsePluginResultJson(const std::string& text) {
    try {
        return Result<std::any>::Ok(std::any(Json::parse(text)));
    } catch (const Json::parse_error& e) {
        return Result<std::any>::Fail(Error{
            .code = ErrorCode::ParseError,
            .message = std::string("plugin returned invalid JSON: ") + e.what(),
            .module = "Core.Plugin.BackendAdapter",
        });
    }
}

} // namespace

// The one function pointer flowing the opposite direction across the ABI
// (Plugin calling Core) -- extern "C" so its calling convention/linkage
// is unambiguous to whatever compiler built the Plugin DLL, matching
// every function pointer type AbiV1.h itself declares. Deliberately at
// aistudio::core scope, not inside the anonymous namespace above --
// PluginBackendAdapter's own friend declaration names it as
// aistudio::core::PluginHostPublishEventTrampoline specifically, which an
// anonymous-namespace definition would not satisfy (it would be a
// different, unrelated entity as far as the friend grant is concerned).
extern "C" void PluginHostPublishEventTrampoline(void* host_context, AistudioPluginString event_name,
                                                  AistudioPluginString payload_json) {
    static_cast<PluginBackendAdapter*>(host_context)->PublishEvent(event_name, payload_json);
}

PluginBackendAdapter::PluginBackendAdapter(const AistudioPluginBackendVTable* vtable)
    : vtable_(vtable), instance_(vtable->create()) {
    if (vtable_->bind_host != nullptr) {
        host_services_.host_context = this;
        host_services_.publish_event = &PluginHostPublishEventTrampoline;
        vtable_->bind_host(instance_, &host_services_);
    }
}

void PluginBackendAdapter::PublishEvent(AistudioPluginString event_name, AistudioPluginString payload_json) {
    Event event;
    event.source = Id();
    event.name = ToStdString(event_name);

    if (payload_json.len > 0) {
        const auto text = ToStdString(payload_json);
        try {
            event.payload = std::any(Json::parse(text));
        } catch (const Json::parse_error& e) {
            AISTUDIO_LOG_WARN("Core.Plugin.BackendAdapter", "publish_event from '" + event.source +
                                                                  "' ignored -- invalid JSON payload: " + e.what());
            return;
        }
    }

    EventBus::Instance().Publish(event);
}

PluginBackendAdapter::~PluginBackendAdapter() {
    if (instance_ != nullptr) {
        vtable_->destroy(instance_);
    }
}

std::string PluginBackendAdapter::Id() const { return ToStdString(vtable_->id(instance_)); }
std::string PluginBackendAdapter::Name() const { return ToStdString(vtable_->name(instance_)); }
std::string PluginBackendAdapter::Version() const { return ToStdString(vtable_->version(instance_)); }

std::vector<std::string> PluginBackendAdapter::Capabilities() const {
    std::vector<std::string> capabilities;
    const auto count = vtable_->capability_count(instance_);
    capabilities.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        capabilities.push_back(ToStdString(vtable_->capability_at(instance_, i)));
    }
    return capabilities;
}

BackendHealth PluginBackendAdapter::Health() const { return MapHealth(vtable_->health(instance_)); }

Result<void> PluginBackendAdapter::Start() {
    AistudioPluginString out_error{};
    if (vtable_->start(instance_, &out_error) != 0) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = ToStdString(out_error),
            .module = "Core.Plugin.BackendAdapter",
        });
    }
    return Result<void>::Ok();
}

Result<void> PluginBackendAdapter::Stop() {
    AistudioPluginString out_error{};
    if (vtable_->stop(instance_, &out_error) != 0) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = ToStdString(out_error),
            .module = "Core.Plugin.BackendAdapter",
        });
    }
    return Result<void>::Ok();
}

Result<void> PluginBackendAdapter::Configure(const Config& config) {
    Json config_json = Json::object();
    for (const auto& [key, value] : config.All()) {
        config_json[key] = value;
    }
    const auto config_text = config_json.dump();

    AistudioPluginString out_error{};
    if (vtable_->configure(instance_, ToPluginString(config_text), &out_error) != 0) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = ToStdString(out_error),
            .module = "Core.Plugin.BackendAdapter",
        });
    }
    return Result<void>::Ok();
}

CommandResult PluginBackendAdapter::Dispatch(const Command& command) {
    const Json command_json = Json{
        {"name", command.name},
        {"request_id", command.request_id},
        {"correlation_id", command.correlation_id},
        {"payload", AnyToPluginPayloadString(command.payload)},
    };
    const auto command_text = command_json.dump();

    AistudioPluginString out_payload_or_error{};
    const auto rc = vtable_->dispatch(instance_, ToPluginString(command_text), &out_payload_or_error);
    const auto result_text = ToStdString(out_payload_or_error);
    if (rc != 0) {
        return CommandResult::Fail(Error{
            .code = ErrorCode::Internal,
            .message = result_text,
            .module = "Core.Plugin.BackendAdapter",
        });
    }
    return ParsePluginResultJson(result_text);
}

QueryResult PluginBackendAdapter::Handle(const Query& query) {
    const Json query_json = Json{
        {"name", query.name},
        {"request_id", query.request_id},
        {"correlation_id", query.correlation_id},
        {"payload", AnyToPluginPayloadString(query.parameters)},
    };
    const auto query_text = query_json.dump();

    AistudioPluginString out_payload_or_error{};
    const auto rc = vtable_->handle(instance_, ToPluginString(query_text), &out_payload_or_error);
    const auto result_text = ToStdString(out_payload_or_error);
    if (rc != 0) {
        return QueryResult::Fail(Error{
            .code = ErrorCode::Internal,
            .message = result_text,
            .module = "Core.Plugin.BackendAdapter",
        });
    }
    return ParsePluginResultJson(result_text);
}

std::vector<ContextItem> PluginBackendAdapter::ProvideContext(const std::string& intent) const {
    if (vtable_->provide_context == nullptr) {
        return {};
    }

    const auto intent_text = Json{{"intent", intent}}.dump();
    AistudioPluginString out_items_or_error{};
    const auto rc = vtable_->provide_context(instance_, ToPluginString(intent_text), &out_items_or_error);
    const auto text = ToStdString(out_items_or_error);
    if (rc != 0) {
        AISTUDIO_LOG_WARN("Core.Plugin.BackendAdapter", "provide_context from '" + Id() + "' failed: " + text);
        return {};
    }

    Json parsed;
    try {
        parsed = Json::parse(text);
    } catch (const Json::parse_error& e) {
        AISTUDIO_LOG_WARN("Core.Plugin.BackendAdapter",
                           "provide_context from '" + Id() + "' returned invalid JSON: " + e.what());
        return {};
    }
    if (!parsed.is_array()) {
        AISTUDIO_LOG_WARN("Core.Plugin.BackendAdapter", "provide_context from '" + Id() + "' did not return a JSON array");
        return {};
    }

    std::vector<ContextItem> items;
    items.reserve(parsed.size());
    for (const auto& entry : parsed) {
        if (!entry.is_object() || !entry.contains("id") || !entry.contains("content")) {
            continue; // skip a malformed entry rather than discard the whole batch over it
        }
        ContextItem item;
        item.id = entry.value("id", std::string());
        item.content = entry.value("content", std::string());
        item.source = ContextSourceKind::Custom;
        item.priority = entry.value("priority", 0);
        item.estimated_tokens = EstimateTokens(item.content); // never trust a Plugin-supplied count
        items.push_back(std::move(item));
    }
    return items;
}

UiPanelDescription PluginBackendAdapter::RenderUiDescription() const {
    if (vtable_->render_ui == nullptr) {
        return {};
    }

    AistudioPluginString out_ui_or_error{};
    const auto rc = vtable_->render_ui(instance_, &out_ui_or_error);
    const auto text = ToStdString(out_ui_or_error);
    if (rc != 0) {
        AISTUDIO_LOG_WARN("Core.Plugin.BackendAdapter", "render_ui from '" + Id() + "' failed: " + text);
        return {};
    }

    Json parsed;
    try {
        parsed = Json::parse(text);
    } catch (const Json::parse_error& e) {
        AISTUDIO_LOG_WARN("Core.Plugin.BackendAdapter",
                           "render_ui from '" + Id() + "' returned invalid JSON: " + e.what());
        return {};
    }
    if (!parsed.is_object()) {
        AISTUDIO_LOG_WARN("Core.Plugin.BackendAdapter", "render_ui from '" + Id() + "' did not return a JSON object");
        return {};
    }

    UiPanelDescription panel;
    panel.title = parsed.value("title", std::string());
    if (parsed.contains("elements") && parsed["elements"].is_array()) {
        for (const auto& entry : parsed["elements"]) {
            if (!entry.is_object() || !entry.contains("kind")) {
                continue; // skip a malformed entry rather than discard the whole panel over it
            }
            const auto kind = entry.value("kind", std::string());
            UiElement element;
            if (kind == "text") {
                element.kind = UiElementKind::Text;
                element.text = entry.value("text", std::string());
            } else if (kind == "separator") {
                element.kind = UiElementKind::Separator;
            } else if (kind == "button") {
                element.kind = UiElementKind::Button;
                element.text = entry.value("text", std::string());
                element.action_id = entry.value("action_id", std::string());
            } else {
                continue; // unrecognized kind -- skip rather than guess
            }
            panel.elements.push_back(std::move(element));
        }
    }
    return panel;
}

Result<std::unique_ptr<IBackend>> LoadPluginBackend(const PluginLoader& loader, const std::string& id) {
    void* symbol = loader.ResolveSymbol(id, "AistudioPluginEntry");
    if (symbol == nullptr) {
        return Result<std::unique_ptr<IBackend>>::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "plugin '" + id + "' is not loaded or does not export AistudioPluginEntry",
            .module = "Core.Plugin.BackendAdapter",
        });
    }

    const auto entry = reinterpret_cast<AistudioPluginEntryFn>(symbol);
    const AistudioPluginBackendVTable* vtable = entry(AISTUDIO_PLUGIN_ABI_VERSION);
    if (vtable == nullptr) {
        return Result<std::unique_ptr<IBackend>>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "plugin '" + id + "' rejected host ABI version " + std::to_string(AISTUDIO_PLUGIN_ABI_VERSION),
            .module = "Core.Plugin.BackendAdapter",
        });
    }
    if (vtable->abi_version != AISTUDIO_PLUGIN_ABI_VERSION) {
        return Result<std::unique_ptr<IBackend>>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "plugin '" + id + "' vtable abi_version " + std::to_string(vtable->abi_version) +
                       " does not match host " + std::to_string(AISTUDIO_PLUGIN_ABI_VERSION),
            .module = "Core.Plugin.BackendAdapter",
        });
    }
    if (vtable->create == nullptr || vtable->destroy == nullptr || vtable->id == nullptr ||
        vtable->name == nullptr || vtable->version == nullptr || vtable->capability_count == nullptr ||
        vtable->capability_at == nullptr || vtable->health == nullptr || vtable->start == nullptr ||
        vtable->stop == nullptr || vtable->configure == nullptr || vtable->dispatch == nullptr ||
        vtable->handle == nullptr) {
        return Result<std::unique_ptr<IBackend>>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "plugin '" + id + "' vtable has one or more NULL function pointers",
            .module = "Core.Plugin.BackendAdapter",
        });
    }

    return Result<std::unique_ptr<IBackend>>::Ok(std::make_unique<PluginBackendAdapter>(vtable));
}

} // namespace aistudio::core
