#include "test_framework.hpp"
#include "Core/Session/GenericCliAdapter.hpp"
#include "Core/Session/HandoffResumeOrchestrator.hpp"

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

std::string WaitForOutputContaining(ICliAdapter& adapter, std::string_view needle,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    std::string accumulated;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        accumulated += adapter.TakeOutput();
        if (accumulated.find(needle) != std::string::npos) {
            return accumulated;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return accumulated;
}

HandoffRecord MakeRecord() {
    HandoffRecord record;
    record.task_id = "t1";
    record.next_steps = "AISTUDIO_HANDOFF_NEXT_STEPS_MARKER";
    return record;
}

} // namespace

AISTUDIO_TEST(BuildHandoffPrompt_EmptyRecord_OnlyHeaderLine) {
    HandoffRecord record;
    const auto prompt = BuildHandoffPrompt(record);
    AISTUDIO_EXPECT(prompt.find("引き継ぎ情報") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("task_id") == std::string::npos);
    AISTUDIO_EXPECT(prompt.find("検証結果") == std::string::npos);
}

AISTUDIO_TEST(BuildHandoffPrompt_PopulatedFields_AreAllIncluded) {
    HandoffRecord record;
    record.task_id = "t1";
    record.base_commit_sha = "abc123";
    record.prerequisites = "前提テキスト";
    record.change_summary = "変更要約";
    record.rationale = "理由";
    record.unresolved_items = "未解決";
    record.next_steps = "次の作業内容";

    const auto prompt = BuildHandoffPrompt(record);
    AISTUDIO_EXPECT(prompt.find("t1") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("abc123") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("前提テキスト") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("変更要約") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("理由") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("未解決") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("次の作業内容") != std::string::npos);
}

AISTUDIO_TEST(BuildHandoffPrompt_Verifications_SummarizesCommandAndOutcomeOnly) {
    HandoffRecord record;
    HandoffVerification verification;
    verification.command = "aistudio_core_tests.exe";
    verification.verified_outcome = HandoffVerificationOutcome::Succeeded;
    verification.verified_output = "AISTUDIO_FULL_OUTPUT_SHOULD_NOT_APPEAR_IN_PROMPT";
    record.verifications.push_back(verification);

    const auto prompt = BuildHandoffPrompt(record);
    AISTUDIO_EXPECT(prompt.find("aistudio_core_tests.exe") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("Succeeded") != std::string::npos);
    AISTUDIO_EXPECT(prompt.find("AISTUDIO_FULL_OUTPUT_SHOULD_NOT_APPEAR_IN_PROMPT") == std::string::npos);
}

AISTUDIO_TEST(ShouldAutoResume_ToggleDisabled_ReturnsFalse) {
    HandoffResumeOrchestrator orchestrator({.auto_trigger_enabled = false});
    CliCapabilities caps;
    AISTUDIO_EXPECT(!orchestrator.ShouldAutoResume(caps, SessionState::Failed));
}

AISTUDIO_TEST(ShouldAutoResume_TerminatedCleanly_ReturnsFalse) {
    HandoffResumeOrchestrator orchestrator({.auto_trigger_enabled = true});
    CliCapabilities caps;
    AISTUDIO_EXPECT(!orchestrator.ShouldAutoResume(caps, SessionState::Terminated));
}

AISTUDIO_TEST(ShouldAutoResume_ResumeSupported_ReturnsFalse) {
    HandoffResumeOrchestrator orchestrator({.auto_trigger_enabled = true});
    CliCapabilities caps;
    caps.resume_conversation = SupportLevel::Supported;
    AISTUDIO_EXPECT(!orchestrator.ShouldAutoResume(caps, SessionState::Failed));
}

AISTUDIO_TEST(ShouldAutoResume_EnabledFailedResumeNotSupported_ReturnsTrue) {
    HandoffResumeOrchestrator orchestrator({.auto_trigger_enabled = true});
    CliCapabilities caps; // Unknown by default
    AISTUDIO_EXPECT(orchestrator.ShouldAutoResume(caps, SessionState::Failed));
}

AISTUDIO_TEST(ResumeViaHandoff_StartsNewSessionAndSendsPrompt) {
    SessionManager manager;
    HandoffResumeOrchestrator orchestrator;

    const auto result = orchestrator.ResumeViaHandoff(manager, MakeRecord(), MakeSession("resumed"),
                                                        MakeProfile(L"cmd.exe"), std::make_unique<GenericCliAdapter>("test-cli"));
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().state == SessionState::Running);

    ICliAdapter* adapter = manager.Adapter("resumed");
    AISTUDIO_EXPECT(adapter != nullptr);
    const auto output = WaitForOutputContaining(*adapter, "AISTUDIO_HANDOFF_NEXT_STEPS_MARKER");
    AISTUDIO_EXPECT(output.find("AISTUDIO_HANDOFF_NEXT_STEPS_MARKER") != std::string::npos);

    manager.Stop("resumed");
}

AISTUDIO_TEST(ObservePollResult_FailedSessionWithHandoffAndAutoTriggerOn_StartsResumeSession) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 3"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    const auto changed = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            auto ids = manager.PollAll();
            if (!ids.empty()) {
                return ids;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return std::vector<std::string>{};
    }();
    AISTUDIO_EXPECT(changed.size() == 1);
    AISTUDIO_EXPECT(manager.Find("s1")->state == SessionState::Failed);

    HandoffResumeOrchestrator orchestrator({.auto_trigger_enabled = true});
    const auto results = orchestrator.ObservePollResult(
        manager, changed, [](const std::string& id) -> std::optional<HandoffRecord> {
            AISTUDIO_EXPECT(id == "s1");
            return MakeRecord();
        },
        [] { return std::make_unique<GenericCliAdapter>("test-cli"); },
        [](const Session&) { return MakeProfile(L"cmd.exe"); });

    AISTUDIO_EXPECT(results.size() == 1);
    AISTUDIO_EXPECT(results[0].IsOk());
    manager.Stop(results[0].Value().id);
}

AISTUDIO_TEST(ObservePollResult_AutoTriggerOff_SkipsSession) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 3"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));
    const auto changed = manager.PollAll(); // already terminal from the wait above -- no new change, but call for shape

    HandoffResumeOrchestrator orchestrator; // auto_trigger_enabled = false
    const auto results = orchestrator.ObservePollResult(
        manager, {"s1"}, [](const std::string&) -> std::optional<HandoffRecord> { return MakeRecord(); },
        [] { return std::make_unique<GenericCliAdapter>("test-cli"); },
        [](const Session&) { return MakeProfile(L"cmd.exe"); });

    AISTUDIO_EXPECT(results.empty());
    (void)changed;
}

AISTUDIO_TEST(ObservePollResult_NoHandoffRecordFound_SkipsSession) {
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 3"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    HandoffResumeOrchestrator orchestrator({.auto_trigger_enabled = true});
    const auto results = orchestrator.ObservePollResult(
        manager, {"s1"}, [](const std::string&) -> std::optional<HandoffRecord> { return std::nullopt; },
        [] { return std::make_unique<GenericCliAdapter>("test-cli"); },
        [](const Session&) { return MakeProfile(L"cmd.exe"); });

    AISTUDIO_EXPECT(results.empty());
}

AISTUDIO_TEST(ObservePollResult_ResumedSessionAlsoFails_DoesNotChainResumeAgain) {
    // Without this guard, a CLI/profile combination that keeps failing
    // would make ObservePollResult() spawn a new resume session every
    // poll cycle forever -- the chain must stop after one hop.
    SessionManager manager;
    AISTUDIO_EXPECT(manager.CreateSession(MakeSession("s1"), MakeProfile(L"cmd.exe /c exit 3"),
                                           std::make_unique<GenericCliAdapter>("test-cli")));
    AISTUDIO_EXPECT(WaitUntilTerminal(manager, "s1"));

    HandoffResumeOrchestrator orchestrator({.auto_trigger_enabled = true});
    const auto first_results = orchestrator.ObservePollResult(
        manager, {"s1"}, [](const std::string&) -> std::optional<HandoffRecord> { return MakeRecord(); },
        [] { return std::make_unique<GenericCliAdapter>("test-cli"); },
        [](const Session&) { return MakeProfile(L"cmd.exe /c exit 3"); });
    AISTUDIO_EXPECT(first_results.size() == 1);
    AISTUDIO_EXPECT(first_results[0].IsOk());
    const std::string resumed_id = first_results[0].Value().id;

    AISTUDIO_EXPECT(WaitUntilTerminal(manager, resumed_id));
    AISTUDIO_EXPECT(manager.Find(resumed_id)->state == SessionState::Failed);

    const auto second_results = orchestrator.ObservePollResult(
        manager, {resumed_id}, [](const std::string&) -> std::optional<HandoffRecord> { return MakeRecord(); },
        [] { return std::make_unique<GenericCliAdapter>("test-cli"); },
        [](const Session&) { return MakeProfile(L"cmd.exe /c exit 3"); });

    AISTUDIO_EXPECT(second_results.empty());
}
