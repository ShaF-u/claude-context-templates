#include "test_framework.hpp"
#include "Core/Protocol/Protocol.hpp"

#include <any>
#include <set>
#include <string>

using namespace aistudio::core;

AISTUDIO_TEST(RequestIdGenerator_Next_ProducesUniqueIds) {
    std::set<std::string> ids;
    for (int i = 0; i < 100; ++i) {
        ids.insert(RequestIdGenerator::Next());
    }
    AISTUDIO_EXPECT(ids.size() == 100);
}

AISTUDIO_TEST(Command_CarriesCorrelationId) {
    Command command;
    command.request_id = RequestIdGenerator::Next();
    command.correlation_id = "corr-1";
    command.backend_id = "unreal";
    command.name = "blueprint.compile";
    command.payload = std::string("BP_Player");

    AISTUDIO_EXPECT(command.correlation_id == "corr-1");
    AISTUDIO_EXPECT(std::any_cast<std::string>(command.payload) == "BP_Player");
}

AISTUDIO_TEST(Event_CarriesSourceAndCorrelation) {
    Event event;
    event.source = "core.null";
    event.name = "BackendConnected";
    event.correlation_id = "corr-1";

    AISTUDIO_EXPECT(event.source == "core.null");
    AISTUDIO_EXPECT(event.name == "BackendConnected");
    AISTUDIO_EXPECT(event.correlation_id == "corr-1");
}

AISTUDIO_TEST(CommandResult_Ok_HoldsAnyPayload) {
    const auto result = CommandResult::Ok(std::any(std::string("done")));
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(std::any_cast<std::string>(result.Value()) == "done");
}
