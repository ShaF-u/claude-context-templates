#include "Core/Database/ContextAuditRepository.hpp"

#include <cstdint>

namespace aistudio::core {

namespace {

ContextAuditEntry RowToEntry(const Row& row) {
    ContextAuditEntry entry;
    for (const auto& [column, value] : row) {
        if (column == "id") {
            entry.id = AsString(value);
        } else if (column == "item_id") {
            entry.item_id = AsString(value);
        } else if (column == "action") {
            entry.action = static_cast<ContextAuditAction>(AsInt64(value));
        } else if (column == "timestamp") {
            entry.timestamp = AsInt64(value);
        } else if (column == "detail") {
            entry.detail = AsString(value);
        }
    }
    return entry;
}

} // namespace

Result<void> ContextAuditRepository::EnsureSchema() {
    return database_.Execute(
        "CREATE TABLE IF NOT EXISTS context_audit_log ("
        "  id TEXT PRIMARY KEY,"
        "  item_id TEXT NOT NULL,"
        "  action INTEGER NOT NULL,"
        "  timestamp INTEGER NOT NULL,"
        "  detail TEXT NOT NULL DEFAULT ''"
        ");");
}

Result<void> ContextAuditRepository::Save(const ContextAuditEntry& entry) {
    return database_.Execute(
        "INSERT INTO context_audit_log (id, item_id, action, timestamp, detail) VALUES (?, ?, ?, ?, ?);",
        {
            entry.id,
            entry.item_id,
            static_cast<std::int64_t>(entry.action),
            entry.timestamp,
            entry.detail,
        });
}

Result<std::vector<ContextAuditEntry>> ContextAuditRepository::FindAll() {
    const auto rows_result = database_.Query("SELECT * FROM context_audit_log ORDER BY rowid;");
    if (!rows_result) {
        return Result<std::vector<ContextAuditEntry>>::Fail(rows_result.Err());
    }
    std::vector<ContextAuditEntry> entries;
    entries.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        entries.push_back(RowToEntry(row));
    }
    return Result<std::vector<ContextAuditEntry>>::Ok(std::move(entries));
}

Result<std::vector<ContextAuditEntry>> ContextAuditRepository::FindByItemId(const std::string& item_id) {
    const auto rows_result =
        database_.Query("SELECT * FROM context_audit_log WHERE item_id = ? ORDER BY rowid;", {item_id});
    if (!rows_result) {
        return Result<std::vector<ContextAuditEntry>>::Fail(rows_result.Err());
    }
    std::vector<ContextAuditEntry> entries;
    entries.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        entries.push_back(RowToEntry(row));
    }
    return Result<std::vector<ContextAuditEntry>>::Ok(std::move(entries));
}

} // namespace aistudio::core
