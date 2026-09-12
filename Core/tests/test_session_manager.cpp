#include "test_framework.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Session/GenericCliAdapter.hpp"
#include "Core/Session/ResourceLeaseLedger.hpp"
#include "Core/Session/SessionManager.hpp"

#include <any>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

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

AISTUDIO_TEST(SessionManager_CreateSession_SuccessfulStart_IsRunning) {
    SessionManager manager;
    auto result = manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli"));
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().state == SessionState::Running);

    const auto found = manager.Find("s1");
    AISTUDIO_EXPECT(found.has_value());
    AISTUDIO_EXPECT(found->state == SessionState::Running);

    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_CreateSession_FailedStart_IsFailedNotAnErrorResult) {
    SessionManager manager;
    auto result = manager.CreateSession(MakeSession("s1"), MakeProfile(L"this_executable_does_not_exist_anywhere.exe"),
                                         std::make_unique<GenericCliAdapter>("test-cli"));
    // A CLI that fails to launch is a normal, expected outcome -- not a
    // SessionManager-level error (same "non-zero exit isn't RunProcess's
    // own failure" convention ProcessRunner already established).
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().state == SessionState::Failed);
    AISTUDIO_EXPECT(result.Value().ended_at != 0);
}

AISTUDIO_TEST(SessionManager_CreateSession_DuplicateId_Fails) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));
    const auto second = manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli"));
    AISTUDIO_EXPECT(second.IsError());
    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_CreateSession_EmptyId_Fails) {
    SessionManager manager;
    const auto result = manager.CreateSession(MakeSession(""), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli"));
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(SessionManager_Stop_MarksTerminatedRegardlessOfWhetherProcessWasStillRunning) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c ping -n 10 127.0.0.1 > NUL"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.Stop("s1"));

    const auto session = manager.Find("s1");
    AISTUDIO_EXPECT(session.has_value());
    AISTUDIO_EXPECT(session->state == SessionState::Terminated);
    AISTUDIO_EXPECT(!manager.Adapter("s1")->IsRunning());
}

AISTUDIO_TEST(SessionManager_Stop_UnknownId_Fails) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.Stop("does_not_exist").IsError());
}

AISTUDIO_TEST(SessionManager_PollAll_OrganicCleanExit_BecomesTerminated) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 0"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));
    AISTUDIO_EXPECT(manager.Find("s1")->state == SessionState::Terminated);
}

AISTUDIO_TEST(SessionManager_PollAll_OrganicNonzeroExit_BecomesFailed) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 3"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));
    AISTUDIO_EXPECT(manager.Find("s1")->state == SessionState::Failed);
}

AISTUDIO_TEST(SessionManager_SetState_MovesNonTerminalSessionToRicherState) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.SetState("s1", SessionState::WaitingForApproval));
    AISTUDIO_EXPECT(manager.Find("s1")->state == SessionState::WaitingForApproval);
    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_SetState_CannotSetTerminalStatesDirectly) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.SetState("s1", SessionState::Terminated).IsError());
    AISTUDIO_EXPECT(manager.SetState("s1", SessionState::Failed).IsError());
    AISTUDIO_EXPECT(manager.Find("s1")->state == SessionState::Running);
    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_SetState_AlreadyTerminalSession_Fails) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.Stop("s1"));
    AISTUDIO_EXPECT(manager.SetState("s1", SessionState::WaitingForInput).IsError());
}

AISTUDIO_TEST(SessionManager_All_ReturnsEveryRegisteredSessionInCreationOrder) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s2"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));

    const auto all = manager.All();
    AISTUDIO_EXPECT(all.size() == 2);
    AISTUDIO_EXPECT(all[0].id == "s1");
    AISTUDIO_EXPECT(all[1].id == "s2");

    manager.Stop("s1");
    manager.Stop("s2");
}

AISTUDIO_TEST(SessionManager_Adapter_UnknownId_ReturnsNullptr) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.Adapter("does_not_exist") == nullptr);
}

AISTUDIO_TEST(SessionManager_Adapter_LetsCallerSendInputAndReadOutput) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c echo session_manager_adapter_check"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    ICliAdapter* adapter = manager.Adapter("s1");
    AISTUDIO_EXPECT(adapter != nullptr);

    std::string output;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        output += adapter->TakeOutput();
        if (output.find("session_manager_adapter_check") != std::string::npos) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    AISTUDIO_EXPECT(output.find("session_manager_adapter_check") != std::string::npos);
    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_CreateSession_PublishesSessionStateChanged) {
    Session received;
    bool got_event = false;
    const auto sub_id = EventBus::Instance().Subscribe("SessionStateChanged", [&](const std::any& payload) {
        received = std::any_cast<Session>(payload);
        got_event = true;
    });

    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));

    EventBus::Instance().Unsubscribe("SessionStateChanged", sub_id);
    AISTUDIO_EXPECT(got_event);
    AISTUDIO_EXPECT(received.id == "s1");
    AISTUDIO_EXPECT(received.state == SessionState::Running);

    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_Stop_WithResourceLeaseLedger_ReleasesSessionsLeases) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("build_output", "s1", LeaseMode::Exclusive));

    SessionManager manager(&ledger);
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.Stop("s1"));

    AISTUDIO_EXPECT(ledger.HoldersOf("build_output").empty());
}

AISTUDIO_TEST(SessionManager_PollAll_OrganicExit_WithResourceLeaseLedger_ReleasesSessionsLeases) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("build_output", "s1", LeaseMode::Exclusive));

    SessionManager manager(&ledger);
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 0"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    AISTUDIO_EXPECT(ledger.HoldersOf("build_output").empty());
}

AISTUDIO_TEST(SessionManager_WithoutResourceLeaseLedger_StopStillWorks) {
    // Default nullptr must not be a hidden requirement -- every prior
    // test in this file already relies on this constructor argument
    // being optional.
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.Stop("s1"));
}

AISTUDIO_TEST(SessionManager_Stop_WithResourceLeaseLedger_PromotesQueuedWaiter) {
    ResourceLeaseLedger ledger;
    AISTUDIO_EXPECT(ledger.Acquire("build_output", "s1", LeaseMode::Exclusive));
    const auto queued = ledger.Acquire("build_output", "s2", LeaseMode::Exclusive);
    AISTUDIO_EXPECT(queued.IsOk());
    AISTUDIO_EXPECT(queued.Value().outcome == LeaseAcquireOutcome::Queued);

    bool promoted = false;
    const auto sub_id = EventBus::Instance().Subscribe("ResourceLeaseAcquired", [&](const std::any&) { promoted = true; });

    SessionManager manager(&ledger);
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.Stop("s1"));

    EventBus::Instance().Unsubscribe("ResourceLeaseAcquired", sub_id);
    AISTUDIO_EXPECT(promoted);
    const auto holders = ledger.HoldersOf("build_output");
    AISTUDIO_EXPECT(holders.size() == 1);
    AISTUDIO_EXPECT(holders[0].session_id == "s2");
}

AISTUDIO_TEST(SessionManager_Restart_TerminalSession_RunsAgainOnTheSameAdapter) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 0"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));
    ICliAdapter* const adapter_before = manager.Adapter("s1");

    AISTUDIO_EXPECT(manager.Restart("s1", MakeProfile(L"cmd.exe /c echo restarted_ok")));
    const auto session = manager.Find("s1");
    AISTUDIO_EXPECT(session.has_value());
    AISTUDIO_EXPECT(session->state == SessionState::Running);
    AISTUDIO_EXPECT(session->ended_at == 0);

    // Same adapter object -- Restart() never creates a new session/adapter.
    AISTUDIO_EXPECT(manager.Adapter("s1") == adapter_before);

    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_Restart_AfterExplicitStop_ResetsStopRequestedSoLaterCrashIsFailed) {
    // Stop() sets stop_requested=true; if Restart() failed to reset it
    // back to false, PollAll()'s "clean = stop_requested || exit==0"
    // would misclassify the restarted process's later crash as a clean
    // Terminated instead of Failed.
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c ping -n 10 127.0.0.1 > NUL"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(manager.Stop("s1"));
    {
        const auto session = manager.Find("s1");
        AISTUDIO_EXPECT(session.has_value());
        AISTUDIO_EXPECT(session->state == SessionState::Terminated);
    }

    AISTUDIO_EXPECT(manager.Restart("s1", MakeProfile(L"cmd.exe /c exit 3")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));
    const auto session = manager.Find("s1");
    AISTUDIO_EXPECT(session.has_value());
    AISTUDIO_EXPECT(session->state == SessionState::Failed);
}

AISTUDIO_TEST(SessionManager_Restart_FailedStart_IsFailedNotAnErrorResult) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 0"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    const auto result = manager.Restart("s1", MakeProfile(L"this_executable_does_not_exist_anywhere.exe"));
    AISTUDIO_EXPECT(result.IsOk()); // matches CreateSession's own convention -- a failed launch isn't a Result-level error
    const auto session = manager.Find("s1");
    AISTUDIO_EXPECT(session.has_value());
    AISTUDIO_EXPECT(session->state == SessionState::Failed);
}

AISTUDIO_TEST(SessionManager_Restart_NonTerminalSession_Fails) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli")));

    const auto result = manager.Restart("s1", MakeProfile(L"cmd.exe"));
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::InvalidArgument);

    manager.Stop("s1");
}

AISTUDIO_TEST(SessionManager_Restart_UnknownId_Fails) {
    SessionManager manager;
    const auto result = manager.Restart("does-not-exist", MakeProfile(L"cmd.exe"));
    AISTUDIO_EXPECT(!result.IsOk());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::NotFound);
}

AISTUDIO_TEST(SessionManager_Restart_PublishesSessionStateChanged) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 0"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    std::vector<SessionState> observed_states;
    const auto sub_id = EventBus::Instance().Subscribe("SessionStateChanged", [&](const std::any& payload) {
        observed_states.push_back(std::any_cast<Session>(payload).state);
    });
    AISTUDIO_EXPECT(manager.Restart("s1", MakeProfile(L"cmd.exe")));
    EventBus::Instance().Unsubscribe("SessionStateChanged", sub_id);

    AISTUDIO_EXPECT(!observed_states.empty());
    AISTUDIO_EXPECT(observed_states.back() == SessionState::Running);

    manager.Stop("s1");
}
