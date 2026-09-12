#include "Core/Database/HandoffRepository.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace aistudio::core {

namespace {

using Json = nlohmann::json;

std::string VerificationsToJson(const std::vector<HandoffVerification>& verifications) {
    Json array = Json::array();
    for (const auto& verification : verifications) {
        Json entry;
        entry["command"] = verification.command;
        entry["claimed_summary"] = verification.claimed_summary;
        entry["verified_outcome"] = ToString(verification.verified_outcome);
        entry["verified_output"] = verification.verified_output;
        entry["verified_at"] = verification.verified_at;
        array.push_back(std::move(entry));
    }
    return array.dump();
}

std::vector<HandoffVerification> VerificationsFromJson(const std::string& text) {
    std::vector<HandoffVerification> verifications;
    if (text.empty()) {
        return verifications;
    }
    const Json array = Json::parse(text, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (array.is_discarded() || !array.is_array()) {
        return verifications;
    }
    for (const auto& entry : array) {
        HandoffVerification verification;
        verification.command = entry.value("command", "");
        verification.claimed_summary = entry.value("claimed_summary", "");
        verification.verified_outcome = HandoffVerificationOutcomeFromString(entry.value("verified_outcome", ""));
        verification.verified_output = entry.value("verified_output", "");
        verification.verified_at = entry.value("verified_at", static_cast<std::int64_t>(0));
        verifications.push_back(std::move(verification));
    }
    return verifications;
}

HandoffRecord RowToHandoffRecord(const Row& row) {
    HandoffRecord record;
    for (const auto& [column, value] : row) {
        if (column == "id") {
            record.id = AsString(value);
        } else if (column == "session_id") {
            record.session_id = AsString(value);
        } else if (column == "task_id") {
            record.task_id = AsString(value);
        } else if (column == "base_commit_sha") {
            record.base_commit_sha = AsString(value);
        } else if (column == "prerequisites") {
            record.prerequisites = AsString(value);
        } else if (column == "change_summary") {
            record.change_summary = AsString(value);
        } else if (column == "rationale") {
            record.rationale = AsString(value);
        } else if (column == "unresolved_items") {
            record.unresolved_items = AsString(value);
        } else if (column == "next_steps") {
            record.next_steps = AsString(value);
        } else if (column == "context_snapshot_id") {
            record.context_snapshot_id = AsString(value);
        } else if (column == "verifications") {
            record.verifications = VerificationsFromJson(AsString(value));
        } else if (column == "created_at") {
            record.created_at = AsInt64(value);
        }
    }
    return record;
}

} // namespace

Result<void> HandoffRepository::EnsureSchema() {
    return database_.Execute(
        "CREATE TABLE IF NOT EXISTS handoff_records ("
        "  id TEXT PRIMARY KEY,"
        "  session_id TEXT NOT NULL DEFAULT '',"
        "  task_id TEXT NOT NULL DEFAULT '',"
        "  base_commit_sha TEXT NOT NULL DEFAULT '',"
        "  prerequisites TEXT NOT NULL DEFAULT '',"
        "  change_summary TEXT NOT NULL DEFAULT '',"
        "  rationale TEXT NOT NULL DEFAULT '',"
        "  unresolved_items TEXT NOT NULL DEFAULT '',"
        "  next_steps TEXT NOT NULL DEFAULT '',"
        "  context_snapshot_id TEXT NOT NULL DEFAULT '',"
        "  verifications TEXT NOT NULL DEFAULT '[]',"
        "  created_at INTEGER NOT NULL"
        ");");
}

Result<void> HandoffRepository::Save(const HandoffRecord& record) {
    return database_.Execute(
        "INSERT INTO handoff_records "
        "  (id, session_id, task_id, base_commit_sha, prerequisites, change_summary, rationale, "
        "   unresolved_items, next_steps, context_snapshot_id, verifications, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  session_id = excluded.session_id, task_id = excluded.task_id, "
        "  base_commit_sha = excluded.base_commit_sha, prerequisites = excluded.prerequisites, "
        "  change_summary = excluded.change_summary, rationale = excluded.rationale, "
        "  unresolved_items = excluded.unresolved_items, next_steps = excluded.next_steps, "
        "  context_snapshot_id = excluded.context_snapshot_id, verifications = excluded.verifications, "
        "  created_at = excluded.created_at;",
        {
            record.id,
            record.session_id,
            record.task_id,
            record.base_commit_sha,
            record.prerequisites,
            record.change_summary,
            record.rationale,
            record.unresolved_items,
            record.next_steps,
            record.context_snapshot_id,
            VerificationsToJson(record.verifications),
            record.created_at,
        });
}

Result<std::optional<HandoffRecord>> HandoffRepository::FindById(const std::string& id) {
    const auto rows_result = database_.Query("SELECT * FROM handoff_records WHERE id = ?;", {id});
    if (!rows_result) {
        return Result<std::optional<HandoffRecord>>::Fail(rows_result.Err());
    }
    const auto& rows = rows_result.Value();
    if (rows.empty()) {
        return Result<std::optional<HandoffRecord>>::Ok(std::nullopt);
    }
    return Result<std::optional<HandoffRecord>>::Ok(RowToHandoffRecord(rows.front()));
}

Result<std::vector<HandoffRecord>> HandoffRepository::FindAll() {
    const auto rows_result = database_.Query("SELECT * FROM handoff_records ORDER BY created_at, rowid;");
    if (!rows_result) {
        return Result<std::vector<HandoffRecord>>::Fail(rows_result.Err());
    }
    std::vector<HandoffRecord> records;
    records.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        records.push_back(RowToHandoffRecord(row));
    }
    return Result<std::vector<HandoffRecord>>::Ok(std::move(records));
}

Result<std::vector<HandoffRecord>> HandoffRepository::FindByTaskId(const std::string& task_id) {
    const auto rows_result =
        database_.Query("SELECT * FROM handoff_records WHERE task_id = ? ORDER BY created_at, rowid;", {task_id});
    if (!rows_result) {
        return Result<std::vector<HandoffRecord>>::Fail(rows_result.Err());
    }
    std::vector<HandoffRecord> records;
    records.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        records.push_back(RowToHandoffRecord(row));
    }
    return Result<std::vector<HandoffRecord>>::Ok(std::move(records));
}

Result<void> HandoffRepository::Remove(const std::string& id) {
    return database_.Execute("DELETE FROM handoff_records WHERE id = ?;", {id});
}

} // namespace aistudio::core
