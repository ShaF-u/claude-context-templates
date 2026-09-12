#include "Core/Database/Migrator.hpp"

#include <algorithm>
#include <cstdint>

namespace aistudio::core {

Result<void> Migrator::EnsureMigrationsTable() {
    return database_.Execute(
        "CREATE TABLE IF NOT EXISTS schema_migrations ("
        "  version INTEGER PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  applied_at TEXT NOT NULL DEFAULT (datetime('now'))"
        ");");
}

Result<int> Migrator::CurrentVersion() {
    const auto rows_result = database_.Query("SELECT COALESCE(MAX(version), 0) AS version FROM schema_migrations;");
    if (!rows_result) {
        return Result<int>::Fail(rows_result.Err());
    }
    const auto& rows = rows_result.Value();
    if (rows.empty() || rows.front().empty()) {
        return Result<int>::Ok(0);
    }
    return Result<int>::Ok(static_cast<int>(AsInt64(rows.front().front().second)));
}

Result<void> Migrator::Apply(const std::vector<Migration>& migrations) {
    if (const auto ensure_result = EnsureMigrationsTable(); !ensure_result) {
        return ensure_result;
    }
    const auto current_result = CurrentVersion();
    if (!current_result) {
        return Result<void>::Fail(current_result.Err());
    }
    const int current_version = current_result.Value();

    std::vector<Migration> sorted = migrations;
    std::sort(sorted.begin(), sorted.end(), [](const Migration& a, const Migration& b) { return a.version < b.version; });

    for (const auto& migration : sorted) {
        if (migration.version <= current_version) {
            continue;
        }
        if (const auto begin_result = database_.Begin(); !begin_result) {
            return begin_result;
        }
        if (const auto exec_result = database_.Execute(migration.sql); !exec_result) {
            database_.Rollback();
            return Result<void>::Fail(Error{
                .code = exec_result.Err().code,
                .message = "migration " + std::to_string(migration.version) + " (" + migration.name +
                           ") failed: " + exec_result.Err().message,
                .module = "Core.Database.Migrator",
            });
        }
        const auto record_result = database_.Execute(
            "INSERT INTO schema_migrations (version, name) VALUES (?, ?);",
            {static_cast<std::int64_t>(migration.version), migration.name});
        if (!record_result) {
            database_.Rollback();
            return record_result;
        }
        if (const auto commit_result = database_.Commit(); !commit_result) {
            return commit_result;
        }
    }
    return Result<void>::Ok();
}

} // namespace aistudio::core
