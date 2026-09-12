#include "test_framework.hpp"
#include "Core/Database/ComparisonTrialRepository.hpp"

using namespace aistudio::core;

namespace {

ComparisonTrial MakeTrial(const std::string& id) {
    ComparisonTrial trial;
    trial.id = id;
    trial.comparison_id = "cmp1";
    trial.task_id = "t1";
    trial.base_commit_sha = "abc123";
    trial.cli_name = "claude";
    trial.session_id = "s1";
    trial.workspace_path = "C:/worktrees/" + id;
    trial.handoff_record_id = "h1";
    trial.operation_id = "op1";
    trial.started_at = 100;
    trial.finished_at = 340;
    trial.decision = AdoptionDecision::Adopted;
    trial.decision_rationale = "テストが通り差分も最小だったため";
    trial.decided_at = 400;
    return trial;
}

} // namespace

AISTUDIO_TEST(ComparisonTrialRepository_SaveAndFindById_RoundTrips) {
    Database db;
    db.Open(":memory:");
    ComparisonTrialRepository repo(db);
    repo.EnsureSchema();

    const auto trial = MakeTrial("trial1");
    AISTUDIO_EXPECT(repo.Save(trial));

    const auto found_result = repo.FindById("trial1");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.comparison_id == "cmp1");
    AISTUDIO_EXPECT(loaded.task_id == "t1");
    AISTUDIO_EXPECT(loaded.base_commit_sha == "abc123");
    AISTUDIO_EXPECT(loaded.cli_name == "claude");
    AISTUDIO_EXPECT(loaded.session_id == "s1");
    AISTUDIO_EXPECT(loaded.workspace_path == "C:/worktrees/trial1");
    AISTUDIO_EXPECT(loaded.handoff_record_id == "h1");
    AISTUDIO_EXPECT(loaded.operation_id == "op1");
    AISTUDIO_EXPECT(loaded.started_at == 100);
    AISTUDIO_EXPECT(loaded.finished_at == 340);
    AISTUDIO_EXPECT(loaded.decision == AdoptionDecision::Adopted);
    AISTUDIO_EXPECT(loaded.decision_rationale == "テストが通り差分も最小だったため");
    AISTUDIO_EXPECT(loaded.decided_at == 400);
}

AISTUDIO_TEST(ComparisonTrialRepository_FindById_MissingId_ReturnsNullopt) {
    Database db;
    db.Open(":memory:");
    ComparisonTrialRepository repo(db);
    repo.EnsureSchema();

    const auto found_result = repo.FindById("does_not_exist");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}

AISTUDIO_TEST(ComparisonTrialRepository_Save_UpsertsExistingId) {
    Database db;
    db.Open(":memory:");
    ComparisonTrialRepository repo(db);
    repo.EnsureSchema();

    auto trial = MakeTrial("trial1");
    trial.decision = AdoptionDecision::Undecided;
    trial.decision_rationale = "";
    trial.decided_at = 0;
    AISTUDIO_EXPECT(repo.Save(trial));

    trial.decision = AdoptionDecision::Rejected;
    trial.decision_rationale = "既存テストを壊していた";
    trial.decided_at = 500;
    AISTUDIO_EXPECT(repo.Save(trial));

    const auto found_result = repo.FindById("trial1");
    AISTUDIO_EXPECT(found_result.Value()->decision == AdoptionDecision::Rejected);
    AISTUDIO_EXPECT(found_result.Value()->decision_rationale == "既存テストを壊していた");
}

AISTUDIO_TEST(ComparisonTrialRepository_FindByComparisonId_FiltersToMatching) {
    Database db;
    db.Open(":memory:");
    ComparisonTrialRepository repo(db);
    repo.EnsureSchema();

    auto first = MakeTrial("trial1");
    first.comparison_id = "cmp1";
    AISTUDIO_EXPECT(repo.Save(first));

    auto second = MakeTrial("trial2");
    second.comparison_id = "cmp2";
    AISTUDIO_EXPECT(repo.Save(second));

    const auto result = repo.FindByComparisonId("cmp1");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().size() == 1);
    AISTUDIO_EXPECT(result.Value()[0].id == "trial1");
}

AISTUDIO_TEST(ComparisonTrialRepository_FindAll_ReturnsInStartedOrder) {
    Database db;
    db.Open(":memory:");
    ComparisonTrialRepository repo(db);
    repo.EnsureSchema();

    auto first = MakeTrial("trial1");
    first.started_at = 100;
    AISTUDIO_EXPECT(repo.Save(first));

    auto second = MakeTrial("trial2");
    second.started_at = 200;
    AISTUDIO_EXPECT(repo.Save(second));

    const auto all_result = repo.FindAll();
    AISTUDIO_EXPECT(all_result.IsOk());
    AISTUDIO_EXPECT(all_result.Value().size() == 2);
    AISTUDIO_EXPECT(all_result.Value()[0].id == "trial1");
    AISTUDIO_EXPECT(all_result.Value()[1].id == "trial2");
}

AISTUDIO_TEST(ComparisonTrialRepository_Remove_DeletesRow) {
    Database db;
    db.Open(":memory:");
    ComparisonTrialRepository repo(db);
    repo.EnsureSchema();

    AISTUDIO_EXPECT(repo.Save(MakeTrial("trial1")));
    AISTUDIO_EXPECT(repo.Remove("trial1"));

    const auto found_result = repo.FindById("trial1");
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}

AISTUDIO_TEST(ComparisonTrialRepository_EnsureSchema_IsIdempotent) {
    Database db;
    db.Open(":memory:");
    ComparisonTrialRepository repo(db);
    AISTUDIO_EXPECT(repo.EnsureSchema());
    AISTUDIO_EXPECT(repo.EnsureSchema());
}
