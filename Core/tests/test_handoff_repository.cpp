#include "test_framework.hpp"
#include "Core/Database/HandoffRepository.hpp"

using namespace aistudio::core;

namespace {

HandoffRecord MakeRecord(const std::string& id) {
    HandoffRecord record;
    record.id = id;
    record.session_id = "s1";
    record.task_id = "t1";
    record.base_commit_sha = "abc123";
    record.prerequisites = "前提テキスト";
    record.change_summary = "変更内容の要約";
    record.rationale = "判断理由";
    record.unresolved_items = "未解決事項";
    record.next_steps = "次の作業";
    record.context_snapshot_id = "snap1";
    record.created_at = 100;

    HandoffVerification verification;
    verification.command = "aistudio_core_tests.exe";
    verification.claimed_summary = "全テストがパスしたはず";
    verification.verified_outcome = HandoffVerificationOutcome::Succeeded;
    verification.verified_output = "All tests passed.";
    verification.verified_at = 150;
    record.verifications.push_back(verification);

    return record;
}

} // namespace

AISTUDIO_TEST(HandoffRepository_SaveAndFindById_RoundTrips) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    repo.EnsureSchema();

    const auto record = MakeRecord("h1");
    AISTUDIO_EXPECT(repo.Save(record));

    const auto found_result = repo.FindById("h1");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.session_id == "s1");
    AISTUDIO_EXPECT(loaded.task_id == "t1");
    AISTUDIO_EXPECT(loaded.base_commit_sha == "abc123");
    AISTUDIO_EXPECT(loaded.prerequisites == "前提テキスト");
    AISTUDIO_EXPECT(loaded.change_summary == "変更内容の要約");
    AISTUDIO_EXPECT(loaded.rationale == "判断理由");
    AISTUDIO_EXPECT(loaded.unresolved_items == "未解決事項");
    AISTUDIO_EXPECT(loaded.next_steps == "次の作業");
    AISTUDIO_EXPECT(loaded.context_snapshot_id == "snap1");
    AISTUDIO_EXPECT(loaded.created_at == 100);

    AISTUDIO_EXPECT(loaded.verifications.size() == 1);
    const auto& v = loaded.verifications[0];
    AISTUDIO_EXPECT(v.command == "aistudio_core_tests.exe");
    AISTUDIO_EXPECT(v.claimed_summary == "全テストがパスしたはず");
    AISTUDIO_EXPECT(v.verified_outcome == HandoffVerificationOutcome::Succeeded);
    AISTUDIO_EXPECT(v.verified_output == "All tests passed.");
    AISTUDIO_EXPECT(v.verified_at == 150);
}

AISTUDIO_TEST(HandoffRepository_FindById_MissingId_ReturnsNullopt) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    repo.EnsureSchema();

    const auto found_result = repo.FindById("does_not_exist");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}

AISTUDIO_TEST(HandoffRepository_Save_UpsertsExistingId) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    repo.EnsureSchema();

    auto record = MakeRecord("h1");
    AISTUDIO_EXPECT(repo.Save(record));

    record.next_steps = "更新後の次の作業";
    record.verifications.clear();
    AISTUDIO_EXPECT(repo.Save(record));

    const auto found_result = repo.FindById("h1");
    AISTUDIO_EXPECT(found_result.Value()->next_steps == "更新後の次の作業");
    AISTUDIO_EXPECT(found_result.Value()->verifications.empty());
}

AISTUDIO_TEST(HandoffRepository_FindAll_ReturnsInCreatedOrder) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    repo.EnsureSchema();

    auto first = MakeRecord("h1");
    first.created_at = 100;
    AISTUDIO_EXPECT(repo.Save(first));

    auto second = MakeRecord("h2");
    second.created_at = 200;
    AISTUDIO_EXPECT(repo.Save(second));

    const auto all_result = repo.FindAll();
    AISTUDIO_EXPECT(all_result.IsOk());
    AISTUDIO_EXPECT(all_result.Value().size() == 2);
    AISTUDIO_EXPECT(all_result.Value()[0].id == "h1");
    AISTUDIO_EXPECT(all_result.Value()[1].id == "h2");
}

AISTUDIO_TEST(HandoffRepository_FindByTaskId_FiltersToMatchingTask) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    repo.EnsureSchema();

    auto first = MakeRecord("h1");
    first.task_id = "t1";
    AISTUDIO_EXPECT(repo.Save(first));

    auto second = MakeRecord("h2");
    second.task_id = "t2";
    AISTUDIO_EXPECT(repo.Save(second));

    const auto result = repo.FindByTaskId("t1");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().size() == 1);
    AISTUDIO_EXPECT(result.Value()[0].id == "h1");
}

AISTUDIO_TEST(HandoffRepository_Remove_DeletesRow) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    repo.EnsureSchema();

    AISTUDIO_EXPECT(repo.Save(MakeRecord("h1")));
    AISTUDIO_EXPECT(repo.Remove("h1"));

    const auto found_result = repo.FindById("h1");
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}

AISTUDIO_TEST(HandoffRepository_Save_MultipleVerifications_RoundTripsAllInOrder) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    repo.EnsureSchema();

    auto record = MakeRecord("h1");
    HandoffVerification second_verification;
    second_verification.command = "cmd.exe /c exit 1";
    second_verification.verified_outcome = HandoffVerificationOutcome::Failed;
    second_verification.verified_at = 160;
    record.verifications.push_back(second_verification);

    AISTUDIO_EXPECT(repo.Save(record));

    const auto loaded = repo.FindById("h1").Value();
    AISTUDIO_EXPECT(loaded->verifications.size() == 2);
    AISTUDIO_EXPECT(loaded->verifications[0].verified_outcome == HandoffVerificationOutcome::Succeeded);
    AISTUDIO_EXPECT(loaded->verifications[1].verified_outcome == HandoffVerificationOutcome::Failed);
}

AISTUDIO_TEST(HandoffRepository_EnsureSchema_IsIdempotent) {
    Database db;
    db.Open(":memory:");
    HandoffRepository repo(db);
    AISTUDIO_EXPECT(repo.EnsureSchema());
    AISTUDIO_EXPECT(repo.EnsureSchema());
}
