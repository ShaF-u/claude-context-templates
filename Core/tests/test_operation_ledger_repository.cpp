#include "test_framework.hpp"
#include "Core/Database/OperationLedgerRepository.hpp"

using namespace aistudio::core;

namespace {

OperationRecord MakeRecord(const std::string& id) {
    OperationRecord record;
    record.id = id;
    record.kind = "git.commit";
    record.started_at = 100;
    record.finished_at = 150;
    record.outcome = OperationOutcome::Succeeded;
    record.interruption_reason = "";
    return record;
}

} // namespace

AISTUDIO_TEST(OperationLedgerRepository_SaveAndFindById_RoundTrips) {
    Database db;
    db.Open(":memory:");
    OperationLedgerRepository repo(db);
    repo.EnsureSchema();

    const auto record = MakeRecord("op1");
    AISTUDIO_EXPECT(repo.Save(record));

    const auto found_result = repo.FindById("op1");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.kind == "git.commit");
    AISTUDIO_EXPECT(loaded.started_at == 100);
    AISTUDIO_EXPECT(loaded.finished_at == 150);
    AISTUDIO_EXPECT(loaded.outcome == OperationOutcome::Succeeded);
    AISTUDIO_EXPECT(loaded.interruption_reason.empty());
}

AISTUDIO_TEST(OperationLedgerRepository_UnknownOutcomeWithInterruptionReason_RoundTrips) {
    Database db;
    db.Open(":memory:");
    OperationLedgerRepository repo(db);
    repo.EnsureSchema();

    auto record = MakeRecord("op1");
    record.outcome = OperationOutcome::Unknown;
    record.interruption_reason = "GUI終了";
    AISTUDIO_EXPECT(repo.Save(record));

    const auto loaded = repo.FindById("op1").Value();
    AISTUDIO_EXPECT(loaded->outcome == OperationOutcome::Unknown);
    AISTUDIO_EXPECT(loaded->interruption_reason == "GUI終了");
}

AISTUDIO_TEST(OperationLedgerRepository_FindById_MissingId_ReturnsNullopt) {
    Database db;
    db.Open(":memory:");
    OperationLedgerRepository repo(db);
    repo.EnsureSchema();

    const auto found_result = repo.FindById("does_not_exist");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}

AISTUDIO_TEST(OperationLedgerRepository_Save_UpsertsExistingId) {
    Database db;
    db.Open(":memory:");
    OperationLedgerRepository repo(db);
    repo.EnsureSchema();

    auto record = MakeRecord("op1");
    record.outcome = OperationOutcome::Unknown;
    record.finished_at = 0;
    AISTUDIO_EXPECT(repo.Save(record));

    record.outcome = OperationOutcome::Failed;
    record.finished_at = 200;
    AISTUDIO_EXPECT(repo.Save(record));

    const auto found_result = repo.FindById("op1");
    AISTUDIO_EXPECT(found_result.Value()->outcome == OperationOutcome::Failed);
    AISTUDIO_EXPECT(found_result.Value()->finished_at == 200);
}

AISTUDIO_TEST(OperationLedgerRepository_FindAll_ReturnsInStartedOrder) {
    Database db;
    db.Open(":memory:");
    OperationLedgerRepository repo(db);
    repo.EnsureSchema();

    auto first = MakeRecord("op1");
    first.started_at = 100;
    AISTUDIO_EXPECT(repo.Save(first));

    auto second = MakeRecord("op2");
    second.started_at = 200;
    AISTUDIO_EXPECT(repo.Save(second));

    const auto all_result = repo.FindAll();
    AISTUDIO_EXPECT(all_result.IsOk());
    AISTUDIO_EXPECT(all_result.Value().size() == 2);
    AISTUDIO_EXPECT(all_result.Value()[0].id == "op1");
    AISTUDIO_EXPECT(all_result.Value()[1].id == "op2");
}

AISTUDIO_TEST(OperationLedgerRepository_Remove_DeletesRow) {
    Database db;
    db.Open(":memory:");
    OperationLedgerRepository repo(db);
    repo.EnsureSchema();

    AISTUDIO_EXPECT(repo.Save(MakeRecord("op1")));
    AISTUDIO_EXPECT(repo.Remove("op1"));

    const auto found_result = repo.FindById("op1");
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}

AISTUDIO_TEST(OperationLedgerRepository_EnsureSchema_IsIdempotent) {
    Database db;
    db.Open(":memory:");
    OperationLedgerRepository repo(db);
    AISTUDIO_EXPECT(repo.EnsureSchema());
    AISTUDIO_EXPECT(repo.EnsureSchema());
}
