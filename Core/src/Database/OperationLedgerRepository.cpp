#include "Core/Database/OperationLedgerRepository.hpp"

namespace aistudio::core {

namespace {

OperationRecord RowToOperationRecord(const Row& row) {
    OperationRecord record;
    for (const auto& [column, value] : row) {
        if (column == "id") {
            record.id = AsString(value);
        } else if (column == "kind") {
            record.kind = AsString(value);
        } else if (column == "started_at") {
            record.started_at = AsInt64(value);
        } else if (column == "finished_at") {
            record.finished_at = AsInt64(value);
        } else if (column == "outcome") {
            record.outcome = OperationOutcomeFromString(AsString(value));
        } else if (column == "interruption_reason") {
            record.interruption_reason = AsString(value);
        }
    }
    return record;
}

} // namespace

Result<void> OperationLedgerRepository::EnsureSchema() {
    return database_.Execute(
        "CREATE TABLE IF NOT EXISTS operation_ledger ("
        "  id TEXT PRIMARY KEY,"
        "  kind TEXT NOT NULL DEFAULT '',"
        "  started_at INTEGER NOT NULL,"
        "  finished_at INTEGER NOT NULL DEFAULT 0,"
        "  outcome TEXT NOT NULL DEFAULT 'Unknown',"
        "  interruption_reason TEXT NOT NULL DEFAULT ''"
        ");");
}

Result<void> OperationLedgerRepository::Save(const OperationRecord& record) {
    return database_.Execute(
        "INSERT INTO operation_ledger (id, kind, started_at, finished_at, outcome, interruption_reason) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  kind = excluded.kind, started_at = excluded.started_at, finished_at = excluded.finished_at, "
        "  outcome = excluded.outcome, interruption_reason = excluded.interruption_reason;",
        {
            record.id,
            record.kind,
            record.started_at,
            record.finished_at,
            ToString(record.outcome),
            record.interruption_reason,
        });
}

Result<std::optional<OperationRecord>> OperationLedgerRepository::FindById(const std::string& id) {
    const auto rows_result = database_.Query("SELECT * FROM operation_ledger WHERE id = ?;", {id});
    if (!rows_result) {
        return Result<std::optional<OperationRecord>>::Fail(rows_result.Err());
    }
    const auto& rows = rows_result.Value();
    if (rows.empty()) {
        return Result<std::optional<OperationRecord>>::Ok(std::nullopt);
    }
    return Result<std::optional<OperationRecord>>::Ok(RowToOperationRecord(rows.front()));
}

Result<std::vector<OperationRecord>> OperationLedgerRepository::FindAll() {
    const auto rows_result = database_.Query("SELECT * FROM operation_ledger ORDER BY started_at, rowid;");
    if (!rows_result) {
        return Result<std::vector<OperationRecord>>::Fail(rows_result.Err());
    }
    std::vector<OperationRecord> records;
    records.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        records.push_back(RowToOperationRecord(row));
    }
    return Result<std::vector<OperationRecord>>::Ok(std::move(records));
}

Result<void> OperationLedgerRepository::Remove(const std::string& id) {
    return database_.Execute("DELETE FROM operation_ledger WHERE id = ?;", {id});
}

} // namespace aistudio::core
