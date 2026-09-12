#include "test_framework.hpp"
#include "Core/Backend/BackendFactoryRegistry.hpp"

#include <algorithm>

using namespace aistudio::core;

namespace {

class FactoryTestBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "factory-test-backend"; }
    [[nodiscard]] std::string Name() const override { return "Factory Test Backend"; }
    [[nodiscard]] std::string Version() const override { return "0.0.1"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override { return {"test.capability"}; }
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }
    CommandResult Dispatch(const Command&) override {
        return CommandResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }
    QueryResult Handle(const Query&) override {
        return QueryResult::Fail(Error{.code = ErrorCode::NotFound, .message = "unused"});
    }
};

AISTUDIO_REGISTER_BACKEND(FactoryTestBackend);

} // namespace

AISTUDIO_TEST(BackendFactoryRegistry_CreateAll_IncludesRegisteredBackend) {
    const auto backends = BackendFactoryRegistry::Instance().CreateAll();

    const auto it = std::find_if(backends.begin(), backends.end(), [](const std::shared_ptr<IBackend>& backend) {
        return backend->Id() == "factory-test-backend";
    });
    AISTUDIO_EXPECT(it != backends.end());
    AISTUDIO_EXPECT((*it)->Capabilities().front() == "test.capability");
}

AISTUDIO_TEST(BackendFactoryRegistry_CreateAll_IsStableAcrossCalls) {
    // All AISTUDIO_REGISTER_BACKEND registrations happen once, at static
    // init, before any test runs — so the factory count doesn't change
    // between calls within a single test run.
    const auto first_count = BackendFactoryRegistry::Instance().CreateAll().size();
    const auto second_count = BackendFactoryRegistry::Instance().CreateAll().size();
    AISTUDIO_EXPECT(first_count == second_count);
    AISTUDIO_EXPECT(first_count == BackendFactoryRegistry::Instance().FactoryCount());
}

AISTUDIO_TEST(BackendFactoryRegistry_CreateAll_ProducesIndependentSharedPtrInstances) {
    const auto backends = BackendFactoryRegistry::Instance().CreateAll();
    const auto other = BackendFactoryRegistry::Instance().CreateAll();

    const auto find_by_id = [](const std::vector<std::shared_ptr<IBackend>>& list) {
        return std::find_if(list.begin(), list.end(),
                             [](const std::shared_ptr<IBackend>& b) { return b->Id() == "factory-test-backend"; });
    };
    const auto a = find_by_id(backends);
    const auto b = find_by_id(other);
    AISTUDIO_EXPECT(a != backends.end() && b != other.end());
    AISTUDIO_EXPECT(a->get() != b->get());
}
