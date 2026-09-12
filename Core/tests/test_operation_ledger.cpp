#include "test_framework.hpp"
#include "Core/Session/OperationLedger.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(OperationLedger_Begin_NewId_ReturnsStarted_CreatesUnknownRecord) {
    OperationLedger ledger;
    const auto result = ledger.Begin("op1", "git.commit");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationBeginOutcome::Started);

    const auto record = ledger.Find("op1");
    AISTUDIO_EXPECT(record.has_value());
    AISTUDIO_EXPECT(record->kind == "git.commit");
    AISTUDIO_EXPECT(record->outcome == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(record->finished_at == 0);
    AISTUDIO_EXPECT(record->started_at > 0);
}

AISTUDIO_TEST(OperationLedger_Begin_ExistingSucceededId_ReturnsAlreadyCompleted_DoesNotResetRecord) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));
    AISTUDIO_EXPECT(ledger.Complete("op1", OperationOutcome::Succeeded));

    const auto result = ledger.Begin("op1", "git.commit");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationBeginOutcome::AlreadyCompleted);
    AISTUDIO_EXPECT(ledger.Find("op1")->outcome == OperationOutcome::Succeeded);
}

AISTUDIO_TEST(OperationLedger_Begin_ExistingUnknownUnfinishedId_ReturnsNeedsReconciliation_DoesNotResetRecord) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));
    const auto first_started_at = ledger.Find("op1")->started_at;

    const auto result = ledger.Begin("op1", "git.commit");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationBeginOutcome::NeedsReconciliation);
    AISTUDIO_EXPECT(ledger.Find("op1")->outcome == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(ledger.Find("op1")->started_at == first_started_at);
}

AISTUDIO_TEST(OperationLedger_Begin_ExistingFailedId_ReturnsStarted_ResetsRecordForRetry) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));
    AISTUDIO_EXPECT(ledger.Complete("op1", OperationOutcome::Failed));

    const auto result = ledger.Begin("op1", "git.commit");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationBeginOutcome::Started);

    const auto record = ledger.Find("op1");
    AISTUDIO_EXPECT(record->outcome == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(record->finished_at == 0);
}

AISTUDIO_TEST(OperationLedger_Complete_SetsOutcomeAndFinishedAt) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));
    AISTUDIO_EXPECT(ledger.Complete("op1", OperationOutcome::Succeeded));

    const auto record = ledger.Find("op1");
    AISTUDIO_EXPECT(record->outcome == OperationOutcome::Succeeded);
    AISTUDIO_EXPECT(record->finished_at > 0);
}

AISTUDIO_TEST(OperationLedger_Complete_UnknownOutcomeArgument_Fails) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));
    const auto result = ledger.Complete("op1", OperationOutcome::Unknown);
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(OperationLedger_Complete_UntrackedId_Fails) {
    OperationLedger ledger;
    const auto result = ledger.Complete("does_not_exist", OperationOutcome::Succeeded);
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(OperationLedger_MarkInterrupted_SetsReasonAndFinishedAt_KeepsOutcomeUnknown) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));
    AISTUDIO_EXPECT(ledger.MarkInterrupted("op1", "GUI終了"));

    const auto record = ledger.Find("op1");
    AISTUDIO_EXPECT(record->outcome == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(record->finished_at > 0);
    AISTUDIO_EXPECT(record->interruption_reason == "GUI終了");
}

AISTUDIO_TEST(OperationLedger_Reconcile_ProbeReportsSucceeded_CompletesRecord) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));

    const auto result = ledger.Reconcile("op1", [] { return Result<OperationOutcome>::Ok(OperationOutcome::Succeeded); });
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationOutcome::Succeeded);
    AISTUDIO_EXPECT(ledger.Find("op1")->outcome == OperationOutcome::Succeeded);
}

AISTUDIO_TEST(OperationLedger_Reconcile_ProbeReportsFailed_CompletesRecord) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));

    const auto result = ledger.Reconcile("op1", [] { return Result<OperationOutcome>::Ok(OperationOutcome::Failed); });
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationOutcome::Failed);
    AISTUDIO_EXPECT(ledger.Find("op1")->outcome == OperationOutcome::Failed);
}

AISTUDIO_TEST(OperationLedger_Reconcile_ProbeReportsUnknown_LeavesRecordUntouched) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));

    const auto result = ledger.Reconcile("op1", [] { return Result<OperationOutcome>::Ok(OperationOutcome::Unknown); });
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value() == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(ledger.Find("op1")->outcome == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(ledger.Find("op1")->finished_at == 0);
}

AISTUDIO_TEST(OperationLedger_Reconcile_ProbeFails_PropagatesError_LeavesRecordUnchanged) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));

    const auto result = ledger.Reconcile("op1", [] {
        return Result<OperationOutcome>::Fail(
            Error{.code = ErrorCode::IOError, .message = "probe failed", .module = "test"});
    });
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(ledger.Find("op1")->outcome == OperationOutcome::Unknown);
}

AISTUDIO_TEST(OperationLedger_Reconcile_UntrackedId_Fails) {
    OperationLedger ledger;
    const auto result =
        ledger.Reconcile("does_not_exist", [] { return Result<OperationOutcome>::Ok(OperationOutcome::Succeeded); });
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(OperationLedger_All_ReturnsEveryTrackedRecord) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(ledger.Begin("op1", "git.commit"));
    AISTUDIO_EXPECT(ledger.Begin("op2", "git.worktree.add"));

    const auto all = ledger.All();
    AISTUDIO_EXPECT(all.size() == 2);
}

AISTUDIO_TEST(OperationLedger_Find_UntrackedId_ReturnsNullopt) {
    OperationLedger ledger;
    AISTUDIO_EXPECT(!ledger.Find("does_not_exist").has_value());
}
