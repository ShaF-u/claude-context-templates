#include "test_framework.hpp"
#include "Core/Session/ComparisonBoard.hpp"

using namespace aistudio::core;

namespace {

ComparisonTrial MakeTrial(const std::string& id, const std::string& comparison_id, const std::string& cli_name) {
    ComparisonTrial trial;
    trial.id = id;
    trial.comparison_id = comparison_id;
    trial.task_id = "t1";
    trial.base_commit_sha = "abc123";
    trial.cli_name = cli_name;
    trial.session_id = "s1";
    trial.workspace_path = "C:/worktrees/" + id;
    trial.started_at = 100;
    return trial;
}

} // namespace

AISTUDIO_TEST(ComparisonBoard_RegisterTrial_NewId_Succeeds) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));

    const auto found = board.Find("trial1");
    AISTUDIO_EXPECT(found.has_value());
    AISTUDIO_EXPECT(found->cli_name == "claude");
    AISTUDIO_EXPECT(found->decision == AdoptionDecision::Undecided);
}

AISTUDIO_TEST(ComparisonBoard_RegisterTrial_DuplicateId_Fails) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));
    const auto result = board.RegisterTrial(MakeTrial("trial1", "cmp1", "other-cli"));
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(ComparisonBoard_TrialsForComparison_ReturnsOnlyMatching) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial2", "cmp1", "other-cli")));
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial3", "cmp2", "claude")));

    const auto result = board.TrialsForComparison("cmp1");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().size() == 2);
}

AISTUDIO_TEST(ComparisonBoard_Find_UntrackedId_ReturnsNullopt) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(!board.Find("does_not_exist").has_value());
}

AISTUDIO_TEST(ComparisonBoard_DecideTrial_Adopted_SetsDecisionAndRationale) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));

    AISTUDIO_EXPECT(board.DecideTrial("trial1", AdoptionDecision::Adopted, "テストが通り差分も最小だったため"));

    const auto found = board.Find("trial1");
    AISTUDIO_EXPECT(found->decision == AdoptionDecision::Adopted);
    AISTUDIO_EXPECT(found->decision_rationale == "テストが通り差分も最小だったため");
    AISTUDIO_EXPECT(found->decided_at > 0);
}

AISTUDIO_TEST(ComparisonBoard_DecideTrial_Rejected_SetsDecisionAndRationale) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));

    AISTUDIO_EXPECT(board.DecideTrial("trial1", AdoptionDecision::Rejected, "既存テストを2件壊していた"));

    const auto found = board.Find("trial1");
    AISTUDIO_EXPECT(found->decision == AdoptionDecision::Rejected);
    AISTUDIO_EXPECT(found->decision_rationale == "既存テストを2件壊していた");
}

AISTUDIO_TEST(ComparisonBoard_DecideTrial_UndecidedArgument_Fails) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));
    const auto result = board.DecideTrial("trial1", AdoptionDecision::Undecided, "reason");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(ComparisonBoard_DecideTrial_EmptyRationale_Fails) {
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));
    const auto result = board.DecideTrial("trial1", AdoptionDecision::Adopted, "");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(board.Find("trial1")->decision == AdoptionDecision::Undecided);
}

AISTUDIO_TEST(ComparisonBoard_DecideTrial_UntrackedId_Fails) {
    ComparisonBoard board;
    const auto result = board.DecideTrial("does_not_exist", AdoptionDecision::Adopted, "reason");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(ComparisonTrial_DurationSeconds_NotFinished_ReturnsNullopt) {
    ComparisonTrial trial = MakeTrial("trial1", "cmp1", "claude");
    trial.started_at = 100;
    trial.finished_at = 0;
    AISTUDIO_EXPECT(!trial.DurationSeconds().has_value());
}

AISTUDIO_TEST(ComparisonTrial_DurationSeconds_Finished_ReturnsDifference) {
    ComparisonTrial trial = MakeTrial("trial1", "cmp1", "claude");
    trial.started_at = 100;
    trial.finished_at = 340;
    AISTUDIO_EXPECT(trial.DurationSeconds().has_value());
    AISTUDIO_EXPECT(*trial.DurationSeconds() == 240);
}

AISTUDIO_TEST(ComparisonBoard_DecideTrial_DoesNotRequireConsensusAcrossTrials) {
    // No API here tallies agreement across trials -- each trial's
    // decision is independent and explicit, confirmed by decidiing two
    // trials in the same comparison to opposite outcomes without either
    // call inspecting the other.
    ComparisonBoard board;
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial1", "cmp1", "claude")));
    AISTUDIO_EXPECT(board.RegisterTrial(MakeTrial("trial2", "cmp1", "other-cli")));

    AISTUDIO_EXPECT(board.DecideTrial("trial1", AdoptionDecision::Adopted, "採用"));
    AISTUDIO_EXPECT(board.DecideTrial("trial2", AdoptionDecision::Rejected, "不採用"));

    AISTUDIO_EXPECT(board.Find("trial1")->decision == AdoptionDecision::Adopted);
    AISTUDIO_EXPECT(board.Find("trial2")->decision == AdoptionDecision::Rejected);
}
