#include "test_framework.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Event/EventBus.hpp"

#include <any>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace aistudio::core;

namespace {
class FakeBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "fake"; }
    [[nodiscard]] std::string Name() const override { return "Fake"; }
    [[nodiscard]] std::string Version() const override { return "0.0.1"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override {
        return {"fake.capability", "versioned.capability@2.1.0"};
    }
    [[nodiscard]] BackendHealth Health() const override { return health_; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }

    CommandResult Dispatch(const Command& command) override {
        last_command_ = command;
        if (command.name == "echo") {
            return CommandResult::Ok(command.payload);
        }
        return CommandResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unknown command: " + command.name});
    }

    QueryResult Handle(const Query& query) override {
        if (query.name == "echo") {
            return QueryResult::Ok(query.parameters);
        }
        return QueryResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unknown query: " + query.name});
    }

    Result<void> Configure(const Config& config) override {
        configure_called_ = true;
        configured_studio_name_ = config.GetOr("studio.name", "");
        return Result<void>::Ok();
    }

    BackendHealth health_ = BackendHealth::Healthy;
    bool configure_called_ = false;
    std::string configured_studio_name_;
    std::optional<Command> last_command_;
};

class FailingConfigureBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "failing-configure"; }
    [[nodiscard]] std::string Name() const override { return "Failing Configure"; }
    [[nodiscard]] std::string Version() const override { return "0.0.1"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override { return {}; }
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }
    CommandResult Dispatch(const Command&) override {
        return CommandResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }
    QueryResult Handle(const Query&) override {
        return QueryResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }
    Result<void> Configure(const Config&) override {
        return Result<void>::Fail(Error{.code = ErrorCode::Internal, .message = "boom"});
    }
};

class FailingStartBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "failing-start"; }
    [[nodiscard]] std::string Name() const override { return "Failing Start"; }
    [[nodiscard]] std::string Version() const override { return "0.0.1"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override { return {}; }
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Fail(Error{.code = ErrorCode::Internal, .message = "boom"}); }
    Result<void> Stop() override { return Result<void>::Ok(); }
    CommandResult Dispatch(const Command&) override {
        return CommandResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }
    QueryResult Handle(const Query&) override {
        return QueryResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }
};
} // namespace

AISTUDIO_TEST(BackendRegistry_RegisterAndFind) {
    BackendRegistry registry;
    AISTUDIO_EXPECT(registry.Register(std::make_shared<FakeBackend>()));
    AISTUDIO_EXPECT(registry.Find("fake") != nullptr);
}

AISTUDIO_TEST(BackendRegistry_FindByCapability) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    const auto found = registry.FindByCapability("fake.capability");
    AISTUDIO_EXPECT(found.size() == 1);
}

AISTUDIO_TEST(BackendRegistry_FindByCapability_NoVersionRequired_MatchesVersionedCapability) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    const auto found = registry.FindByCapability("versioned.capability");
    AISTUDIO_EXPECT(found.size() == 1);
}

AISTUDIO_TEST(BackendRegistry_FindByCapability_CompatibleVersionRequired_Matches) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    const auto found = registry.FindByCapability("versioned.capability", CapabilityVersion{2, 0, 0});
    AISTUDIO_EXPECT(found.size() == 1);
}

AISTUDIO_TEST(BackendRegistry_FindByCapability_IncompatibleMajorVersionRequired_NoMatch) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    const auto found = registry.FindByCapability("versioned.capability", CapabilityVersion{3, 0, 0});
    AISTUDIO_EXPECT(found.empty());
}

AISTUDIO_TEST(BackendRegistry_FindByCapability_NewerMinorRequiredThanProvided_NoMatch) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    const auto found = registry.FindByCapability("versioned.capability", CapabilityVersion{2, 9, 0});
    AISTUDIO_EXPECT(found.empty());
}

AISTUDIO_TEST(BackendRegistry_FindByCapability_VersionRequiredForUnversionedCapability_NoMatch) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    const auto found = registry.FindByCapability("fake.capability", CapabilityVersion{1, 0, 0});
    AISTUDIO_EXPECT(found.empty());
}

AISTUDIO_TEST(BackendRegistry_DuplicateRegister_Fails) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    const auto second = registry.Register(std::make_shared<FakeBackend>());
    AISTUDIO_EXPECT(second.IsError());
}

AISTUDIO_TEST(BackendRegistry_Unregister_RemovesBackend) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    registry.Unregister("fake");
    AISTUDIO_EXPECT(registry.Find("fake") == nullptr);
}

AISTUDIO_TEST(BackendRegistry_Dispatch_RoutesToBackend) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    Command command;
    command.backend_id = "fake";
    command.name = "echo";
    command.payload = std::string("hello");

    const auto result = registry.Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<std::string>(result.Value()) == "hello");
}

AISTUDIO_TEST(BackendRegistry_Dispatch_FillsRequestIdWhenBlank) {
    BackendRegistry registry;
    auto backend = std::make_shared<FakeBackend>();
    registry.Register(backend);

    Command command;
    command.backend_id = "fake";
    command.name = "echo";
    AISTUDIO_EXPECT(command.request_id.empty());

    const auto result = registry.Dispatch(command);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(backend->last_command_.has_value());
    AISTUDIO_EXPECT(!backend->last_command_->request_id.empty());

    const auto first_request_id = backend->last_command_->request_id;

    Command command2;
    command2.backend_id = "fake";
    command2.name = "echo";
    const auto result2 = registry.Dispatch(command2);
    AISTUDIO_EXPECT(result2.IsOk());
    AISTUDIO_EXPECT(backend->last_command_->request_id != first_request_id);
}

AISTUDIO_TEST(BackendRegistry_Dispatch_UnknownBackend_Fails) {
    BackendRegistry registry;

    Command command;
    command.backend_id = "does-not-exist";
    command.name = "echo";

    const auto result = registry.Dispatch(command);
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(BackendRegistry_RunQuery_RoutesToBackend) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    Query query;
    query.backend_id = "fake";
    query.name = "echo";
    query.parameters = 42;

    const auto result = registry.RunQuery(query);
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<int>(result.Value()) == 42);
}

AISTUDIO_TEST(BackendRegistry_RunQuery_UnknownBackend_Fails) {
    BackendRegistry registry;

    Query query;
    query.backend_id = "does-not-exist";
    query.name = "echo";

    AISTUDIO_EXPECT(registry.RunQuery(query).IsError());
}

AISTUDIO_TEST(BackendRegistry_ConfigureAll_CallsConfigureOnEachBackend) {
    BackendRegistry registry;
    auto backend = std::make_shared<FakeBackend>();
    registry.Register(backend);

    Config config;
    config.Set("studio.name", "Test Studio");

    AISTUDIO_EXPECT(registry.ConfigureAll(config));
    AISTUDIO_EXPECT(backend->configure_called_);
    AISTUDIO_EXPECT(backend->configured_studio_name_ == "Test Studio");
}

AISTUDIO_TEST(BackendRegistry_ConfigureAll_AggregatesFailures) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FailingConfigureBackend>());

    const auto result = registry.ConfigureAll(Config{});
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().message.find("failing-configure") != std::string::npos);
}

AISTUDIO_TEST(BackendRegistry_CheckHealth_ReturnsCurrentReadings) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    const auto readings = registry.CheckHealth();
    AISTUDIO_EXPECT(readings.size() == 1);
    AISTUDIO_EXPECT(readings[0].first == "fake");
    AISTUDIO_EXPECT(readings[0].second == BackendHealth::Healthy);
}

AISTUDIO_TEST(BackendRegistry_CheckHealth_PublishesEventOnFirstPoll) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    int received = 0;
    const auto subscription_id = EventBus::Instance().Subscribe("BackendHealthChanged", [&](const std::any& payload) {
        if (std::any_cast<BackendHealthChange>(&payload) != nullptr) {
            ++received;
        }
    });

    registry.CheckHealth();

    EventBus::Instance().Unsubscribe("BackendHealthChanged", subscription_id);
    AISTUDIO_EXPECT(received == 1);
}

AISTUDIO_TEST(BackendRegistry_CheckHealth_NoEventWhenUnchanged) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    registry.CheckHealth(); // first poll establishes the baseline

    int received = 0;
    const auto subscription_id = EventBus::Instance().Subscribe("BackendHealthChanged", [&](const std::any& payload) {
        if (std::any_cast<BackendHealthChange>(&payload) != nullptr) {
            ++received;
        }
    });

    registry.CheckHealth(); // second poll, health unchanged

    EventBus::Instance().Unsubscribe("BackendHealthChanged", subscription_id);
    AISTUDIO_EXPECT(received == 0);
}

AISTUDIO_TEST(BackendRegistry_CheckHealth_PublishesEventWhenHealthChanges) {
    BackendRegistry registry;
    auto backend = std::make_shared<FakeBackend>();
    registry.Register(backend);
    registry.CheckHealth(); // baseline: Healthy

    backend->health_ = BackendHealth::Degraded;

    std::vector<BackendHealthChange> received;
    const auto subscription_id = EventBus::Instance().Subscribe("BackendHealthChanged", [&](const std::any& payload) {
        if (const auto* change = std::any_cast<BackendHealthChange>(&payload)) {
            received.push_back(*change);
        }
    });

    registry.CheckHealth();

    EventBus::Instance().Unsubscribe("BackendHealthChanged", subscription_id);
    AISTUDIO_EXPECT(received.size() == 1);
    AISTUDIO_EXPECT(received[0].previous == BackendHealth::Healthy);
    AISTUDIO_EXPECT(received[0].current == BackendHealth::Degraded);
}

AISTUDIO_TEST(BackendRegistry_Register_SetsInitialLifecycleStateToRegistered) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    AISTUDIO_EXPECT(registry.LifecycleState("fake") == BackendLifecycleState::Registered);
}

AISTUDIO_TEST(BackendRegistry_LifecycleState_UnknownId_ReturnsNullopt) {
    BackendRegistry registry;
    AISTUDIO_EXPECT(registry.LifecycleState("does-not-exist") == std::nullopt);
}

AISTUDIO_TEST(BackendRegistry_StartBackend_TransitionsToRunning) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    AISTUDIO_EXPECT(registry.StartBackend("fake"));
    AISTUDIO_EXPECT(registry.LifecycleState("fake") == BackendLifecycleState::Running);
}

AISTUDIO_TEST(BackendRegistry_StartBackend_UnknownId_Fails) {
    BackendRegistry registry;
    const auto result = registry.StartBackend("does-not-exist");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(BackendRegistry_StartBackend_AlreadyRunning_Fails) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    registry.StartBackend("fake");

    const auto result = registry.StartBackend("fake");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(BackendRegistry_StartBackend_FailureTransitionsToFailed) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FailingStartBackend>());

    const auto result = registry.StartBackend("failing-start");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(registry.LifecycleState("failing-start") == BackendLifecycleState::Failed);
}

AISTUDIO_TEST(BackendRegistry_StopBackend_TransitionsToStopped) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    registry.StartBackend("fake");

    AISTUDIO_EXPECT(registry.StopBackend("fake"));
    AISTUDIO_EXPECT(registry.LifecycleState("fake") == BackendLifecycleState::Stopped);
}

AISTUDIO_TEST(BackendRegistry_StopBackend_NotRunning_Fails) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    const auto result = registry.StopBackend("fake");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);
}

AISTUDIO_TEST(BackendRegistry_StartBackend_CanRestartAfterStop) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    registry.StartBackend("fake");
    registry.StopBackend("fake");

    AISTUDIO_EXPECT(registry.StartBackend("fake"));
    AISTUDIO_EXPECT(registry.LifecycleState("fake") == BackendLifecycleState::Running);
}

AISTUDIO_TEST(BackendRegistry_StartAll_StartsEveryBackend) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    AISTUDIO_EXPECT(registry.StartAll());
    AISTUDIO_EXPECT(registry.LifecycleState("fake") == BackendLifecycleState::Running);
}

AISTUDIO_TEST(BackendRegistry_StartAll_AggregatesFailures) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FailingStartBackend>());

    const auto result = registry.StartAll();
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().message.find("failing-start") != std::string::npos);
}

AISTUDIO_TEST(BackendRegistry_StopAll_StopsEveryBackend) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());
    registry.StartAll();

    AISTUDIO_EXPECT(registry.StopAll());
    AISTUDIO_EXPECT(registry.LifecycleState("fake") == BackendLifecycleState::Stopped);
}

AISTUDIO_TEST(BackendRegistry_StartBackend_PublishesLifecycleChangedEvents) {
    BackendRegistry registry;
    registry.Register(std::make_shared<FakeBackend>());

    std::vector<BackendLifecycleChange> received;
    const auto subscription_id =
        EventBus::Instance().Subscribe("BackendLifecycleChanged", [&](const std::any& payload) {
            if (const auto* change = std::any_cast<BackendLifecycleChange>(&payload)) {
                received.push_back(*change);
            }
        });

    registry.StartBackend("fake");

    EventBus::Instance().Unsubscribe("BackendLifecycleChanged", subscription_id);
    AISTUDIO_EXPECT(received.size() == 2);
    AISTUDIO_EXPECT(received[0].previous == BackendLifecycleState::Registered);
    AISTUDIO_EXPECT(received[0].current == BackendLifecycleState::Starting);
    AISTUDIO_EXPECT(received[1].previous == BackendLifecycleState::Starting);
    AISTUDIO_EXPECT(received[1].current == BackendLifecycleState::Running);
}

AISTUDIO_TEST(ToString_BackendHealth_CoversAllValues) {
    AISTUDIO_EXPECT(ToString(BackendHealth::Unknown) == "Unknown");
    AISTUDIO_EXPECT(ToString(BackendHealth::Healthy) == "Healthy");
    AISTUDIO_EXPECT(ToString(BackendHealth::Degraded) == "Degraded");
    AISTUDIO_EXPECT(ToString(BackendHealth::Unavailable) == "Unavailable");
}
