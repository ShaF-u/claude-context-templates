#include "Core/Database/ComparisonTrialRepository.hpp"

namespace aistudio::core {

namespace {

ComparisonTrial RowToTrial(const Row& row) {
    ComparisonTrial trial;
    for (const auto& [column, value] : row) {
        if (column == "id") {
            trial.id = AsString(value);
        } else if (column == "comparison_id") {
            trial.comparison_id = AsString(value);
        } else if (column == "task_id") {
            trial.task_id = AsString(value);
        } else if (column == "base_commit_sha") {
            trial.base_commit_sha = AsString(value);
        } else if (column == "cli_name") {
            trial.cli_name = AsString(value);
        } else if (column == "session_id") {
            trial.session_id = AsString(value);
        } else if (column == "workspace_path") {
            trial.workspace_path = AsString(value);
        } else if (column == "handoff_record_id") {
            trial.handoff_record_id = AsString(value);
        } else if (column == "operation_id") {
            trial.operation_id = AsString(value);
        } else if (column == "started_at") {
            trial.started_at = AsInt64(value);
        } else if (column == "finished_at") {
            trial.finished_at = AsInt64(value);
        } else if (column == "decision") {
            trial.decision = AdoptionDecisionFromString(AsString(value));
        } else if (column == "decision_rationale") {
            trial.decision_rationale = AsString(value);
        } else if (column == "decided_at") {
            trial.decided_at = AsInt64(value);
        }
    }
    return trial;
}

} // namespace

Result<void> ComparisonTrialRepository::EnsureSchema() {
    return database_.Execute(
        "CREATE TABLE IF NOT EXISTS comparison_trials ("
        "  id TEXT PRIMARY KEY,"
        "  comparison_id TEXT NOT NULL DEFAULT '',"
        "  task_id TEXT NOT NULL DEFAULT '',"
        "  base_commit_sha TEXT NOT NULL DEFAULT '',"
        "  cli_name TEXT NOT NULL DEFAULT '',"
        "  session_id TEXT NOT NULL DEFAULT '',"
        "  workspace_path TEXT NOT NULL DEFAULT '',"
        "  handoff_record_id TEXT NOT NULL DEFAULT '',"
        "  operation_id TEXT NOT NULL DEFAULT '',"
        "  started_at INTEGER NOT NULL,"
        "  finished_at INTEGER NOT NULL DEFAULT 0,"
        "  decision TEXT NOT NULL DEFAULT 'Undecided',"
        "  decision_rationale TEXT NOT NULL DEFAULT '',"
        "  decided_at INTEGER NOT NULL DEFAULT 0"
        ");");
}

Result<void> ComparisonTrialRepository::Save(const ComparisonTrial& trial) {
    return database_.Execute(
        "INSERT INTO comparison_trials "
        "  (id, comparison_id, task_id, base_commit_sha, cli_name, session_id, workspace_path, "
        "   handoff_record_id, operation_id, started_at, finished_at, decision, decision_rationale, decided_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  comparison_id = excluded.comparison_id, task_id = excluded.task_id, "
        "  base_commit_sha = excluded.base_commit_sha, cli_name = excluded.cli_name, "
        "  session_id = excluded.session_id, workspace_path = excluded.workspace_path, "
        "  handoff_record_id = excluded.handoff_record_id, operation_id = excluded.operation_id, "
        "  started_at = excluded.started_at, finished_at = excluded.finished_at, "
        "  decision = excluded.decision, decision_rationale = excluded.decision_rationale, "
        "  decided_at = excluded.decided_at;",
        {
            trial.id,
            trial.comparison_id,
            trial.task_id,
            trial.base_commit_sha,
            trial.cli_name,
            trial.session_id,
            trial.workspace_path,
            trial.handoff_record_id,
            trial.operation_id,
            trial.started_at,
            trial.finished_at,
            ToString(trial.decision),
            trial.decision_rationale,
            trial.decided_at,
        });
}

Result<std::optional<ComparisonTrial>> ComparisonTrialRepository::FindById(const std::string& id) {
    const auto rows_result = database_.Query("SELECT * FROM comparison_trials WHERE id = ?;", {id});
    if (!rows_result) {
        return Result<std::optional<ComparisonTrial>>::Fail(rows_result.Err());
    }
    const auto& rows = rows_result.Value();
    if (rows.empty()) {
        return Result<std::optional<ComparisonTrial>>::Ok(std::nullopt);
    }
    return Result<std::optional<ComparisonTrial>>::Ok(RowToTrial(rows.front()));
}

Result<std::vector<ComparisonTrial>> ComparisonTrialRepository::FindByComparisonId(const std::string& comparison_id) {
    const auto rows_result = database_.Query(
        "SELECT * FROM comparison_trials WHERE comparison_id = ? ORDER BY started_at, rowid;", {comparison_id});
    if (!rows_result) {
        return Result<std::vector<ComparisonTrial>>::Fail(rows_result.Err());
    }
    std::vector<ComparisonTrial> trials;
    trials.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        trials.push_back(RowToTrial(row));
    }
    return Result<std::vector<ComparisonTrial>>::Ok(std::move(trials));
}

Result<std::vector<ComparisonTrial>> ComparisonTrialRepository::FindAll() {
    const auto rows_result = database_.Query("SELECT * FROM comparison_trials ORDER BY started_at, rowid;");
    if (!rows_result) {
        return Result<std::vector<ComparisonTrial>>::Fail(rows_result.Err());
    }
    std::vector<ComparisonTrial> trials;
    trials.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        trials.push_back(RowToTrial(row));
    }
    return Result<std::vector<ComparisonTrial>>::Ok(std::move(trials));
}

Result<void> ComparisonTrialRepository::Remove(const std::string& id) {
    return database_.Execute("DELETE FROM comparison_trials WHERE id = ?;", {id});
}

} // namespace aistudio::core
