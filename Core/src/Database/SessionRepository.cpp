#include "Core/Database/SessionRepository.hpp"

namespace aistudio::core {

namespace {

Session RowToSession(const Row& row) {
    Session session;
    for (const auto& [column, value] : row) {
        if (column == "id") {
            session.id = AsString(value);
        } else if (column == "project_id") {
            session.project_id = AsString(value);
        } else if (column == "workspace_id") {
            session.workspace_id = AsString(value);
        } else if (column == "task_id") {
            session.task_id = AsString(value);
        } else if (column == "cli_name") {
            session.cli_name = AsString(value);
        } else if (column == "state") {
            session.state = static_cast<SessionState>(AsInt64(value));
        } else if (column == "created_at") {
            session.created_at = AsInt64(value);
        } else if (column == "started_at") {
            session.started_at = AsInt64(value);
        } else if (column == "ended_at") {
            session.ended_at = AsInt64(value);
        }
    }
    return session;
}

} // namespace

Result<void> SessionRepository::EnsureSchema() {
    return database_.Execute(
        "CREATE TABLE IF NOT EXISTS sessions ("
        "  id TEXT PRIMARY KEY,"
        "  project_id TEXT NOT NULL DEFAULT '',"
        "  workspace_id TEXT NOT NULL DEFAULT '',"
        "  task_id TEXT NOT NULL DEFAULT '',"
        "  cli_name TEXT NOT NULL DEFAULT '',"
        "  state INTEGER NOT NULL,"
        "  created_at INTEGER NOT NULL,"
        "  started_at INTEGER NOT NULL DEFAULT 0,"
        "  ended_at INTEGER NOT NULL DEFAULT 0"
        ");");
}

Result<void> SessionRepository::Save(const Session& session) {
    return database_.Execute(
        "INSERT INTO sessions (id, project_id, workspace_id, task_id, cli_name, state, created_at, started_at, ended_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  project_id = excluded.project_id, workspace_id = excluded.workspace_id, "
        "  task_id = excluded.task_id, cli_name = excluded.cli_name, state = excluded.state, "
        "  created_at = excluded.created_at, started_at = excluded.started_at, ended_at = excluded.ended_at;",
        {
            session.id,
            session.project_id,
            session.workspace_id,
            session.task_id,
            session.cli_name,
            static_cast<std::int64_t>(session.state),
            session.created_at,
            session.started_at,
            session.ended_at,
        });
}

Result<std::optional<Session>> SessionRepository::FindById(const std::string& id) {
    const auto rows_result = database_.Query("SELECT * FROM sessions WHERE id = ?;", {id});
    if (!rows_result) {
        return Result<std::optional<Session>>::Fail(rows_result.Err());
    }
    const auto& rows = rows_result.Value();
    if (rows.empty()) {
        return Result<std::optional<Session>>::Ok(std::nullopt);
    }
    return Result<std::optional<Session>>::Ok(RowToSession(rows.front()));
}

Result<std::vector<Session>> SessionRepository::FindAll() {
    const auto rows_result = database_.Query("SELECT * FROM sessions ORDER BY created_at, rowid;");
    if (!rows_result) {
        return Result<std::vector<Session>>::Fail(rows_result.Err());
    }
    std::vector<Session> sessions;
    sessions.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        sessions.push_back(RowToSession(row));
    }
    return Result<std::vector<Session>>::Ok(std::move(sessions));
}

Result<void> SessionRepository::Remove(const std::string& id) {
    return database_.Execute("DELETE FROM sessions WHERE id = ?;", {id});
}

} // namespace aistudio::core
