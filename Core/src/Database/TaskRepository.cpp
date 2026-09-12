#include "Core/Database/TaskRepository.hpp"

#include "Core/Database/StringList.hpp"

#include <cstdint>

namespace aistudio::core {

namespace {

Task RowToTask(const Row& row) {
    Task task;
    for (const auto& [column, value] : row) {
        if (column == "id") {
            task.id = AsString(value);
        } else if (column == "name") {
            task.name = AsString(value);
        } else if (column == "state") {
            task.state = static_cast<TaskState>(AsInt64(value));
        } else if (column == "progress") {
            task.progress = AsDouble(value);
        } else if (column == "retry_count") {
            task.retry_count = static_cast<int>(AsInt64(value));
        } else if (column == "max_retries") {
            task.max_retries = static_cast<int>(AsInt64(value));
        } else if (column == "depends_on") {
            task.depends_on = SplitStringList(AsString(value));
        }
    }
    return task;
}

} // namespace

Result<void> TaskRepository::EnsureSchema() {
    return database_.Execute(
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id TEXT PRIMARY KEY,"
        "  name TEXT NOT NULL,"
        "  state INTEGER NOT NULL,"
        "  progress REAL NOT NULL,"
        "  retry_count INTEGER NOT NULL,"
        "  max_retries INTEGER NOT NULL,"
        "  depends_on TEXT NOT NULL DEFAULT ''"
        ");");
}

Result<void> TaskRepository::Save(const Task& task) {
    return database_.Execute(
        "INSERT INTO tasks (id, name, state, progress, retry_count, max_retries, depends_on) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  name = excluded.name, state = excluded.state, progress = excluded.progress, "
        "  retry_count = excluded.retry_count, max_retries = excluded.max_retries, "
        "  depends_on = excluded.depends_on;",
        {
            task.id,
            task.name,
            static_cast<std::int64_t>(task.state),
            task.progress,
            static_cast<std::int64_t>(task.retry_count),
            static_cast<std::int64_t>(task.max_retries),
            JoinStringList(task.depends_on),
        });
}

Result<std::optional<Task>> TaskRepository::FindById(const std::string& id) {
    const auto rows_result = database_.Query("SELECT * FROM tasks WHERE id = ?;", {id});
    if (!rows_result) {
        return Result<std::optional<Task>>::Fail(rows_result.Err());
    }
    const auto& rows = rows_result.Value();
    if (rows.empty()) {
        return Result<std::optional<Task>>::Ok(std::nullopt);
    }
    return Result<std::optional<Task>>::Ok(RowToTask(rows.front()));
}

Result<std::vector<Task>> TaskRepository::FindAll() {
    const auto rows_result = database_.Query("SELECT * FROM tasks ORDER BY rowid;");
    if (!rows_result) {
        return Result<std::vector<Task>>::Fail(rows_result.Err());
    }
    std::vector<Task> tasks;
    tasks.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        tasks.push_back(RowToTask(row));
    }
    return Result<std::vector<Task>>::Ok(std::move(tasks));
}

Result<void> TaskRepository::Remove(const std::string& id) {
    return database_.Execute("DELETE FROM tasks WHERE id = ?;", {id});
}

} // namespace aistudio::core
