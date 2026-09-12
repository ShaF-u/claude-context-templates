#include "test_framework.hpp"
#include "Core/Session/GenericCliAdapter.hpp"
#include "Core/Session/OperationProbes.hpp"

#include <chrono>
#include <thread>

using namespace aistudio::core;

namespace {

Session MakeSession(std::string id) {
    Session session;
    session.id = std::move(id);
    session.cli_name = "test-cli";
    return session;
}

CliProfile MakeProfile(std::wstring command_line) {
    CliProfile profile;
    profile.command_line = std::move(command_line);
    return profile;
}

bool WaitUntilTerminal(SessionManager& manager, const std::string& id,
                        std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        manager.PollAll();
        const auto session = manager.Find(id);
        if (session.has_value() && session->IsTerminal()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

AISTUDIO_TEST(SessionOutcomeProbe_UntrackedSessionId_Fails) {
    SessionManager manager;
    const auto probe = SessionOutcomeProbe(manager, "does_not_exist");
    const auto result = probe();
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(SessionOutcomeProbe_StillRunningSession_ReturnsUnknown) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));

    const auto probe = SessionOutcomeProbe(manager, "s1");
    const auto result = probe();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationOutcome::Unknown);

    manager.Stop("s1");
}

AISTUDIO_TEST(SessionOutcomeProbe_OrganicCleanExit_ReturnsSucceeded) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 0"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    const auto probe = SessionOutcomeProbe(manager, "s1");
    const auto result = probe();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationOutcome::Succeeded);
}

AISTUDIO_TEST(SessionOutcomeProbe_OrganicNonzeroExit_ReturnsFailed) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 3"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    const auto probe = SessionOutcomeProbe(manager, "s1");
    const auto result = probe();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationOutcome::Failed);
}

AISTUDIO_TEST(SessionOutcomeProbe_ExplicitStop_ReturnsSucceeded) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.Stop("s1"));

    const auto probe = SessionOutcomeProbe(manager, "s1");
    const auto result = probe();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationOutcome::Succeeded);
}

AISTUDIO_TEST(SessionOutcomeProbe_IntegratesWithOperationLedgerReconcile) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 0"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));

    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op-for-s1", "session.run"));

    // Reconcile while still running -- Unknown, record untouched.
    const auto first = ledger.Reconcile("op-for-s1", SessionOutcomeProbe(manager, "s1"));
    AISTUDIO_EXPECT(first.IsOk());
    AISTUDIO_EXPECT(first.Value() == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(ledger.Find("op-for-s1")->outcome == OperationOutcome::Unknown);

    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    const auto second = ledger.Reconcile("op-for-s1", SessionOutcomeProbe(manager, "s1"));
    AISTUDIO_EXPECT(second.IsOk());
    AISTUDIO_EXPECT(second.Value() == OperationOutcome::Succeeded);
    AISTUDIO_EXPECT(ledger.Find("op-for-s1")->outcome == OperationOutcome::Succeeded);
}
