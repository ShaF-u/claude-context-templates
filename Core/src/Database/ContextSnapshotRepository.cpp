#include "Core/Database/ContextSnapshotRepository.hpp"

#include "Core/Context/ContextItem.hpp" // ToString(ContextSourceKind) / ContextSourceKindFromString
#include "Core/Database/StringList.hpp"

#include <cstdint>

namespace aistudio::core {

namespace {

ContextSnapshot RowToSnapshot(const Row& row) {
    ContextSnapshot snapshot;
    for (const auto& [column, value] : row) {
        if (column == "id") {
            snapshot.id = AsString(value);
        } else if (column == "task_name") {
            snapshot.task_name = AsString(value);
        } else if (column == "git_commit") {
            snapshot.git_commit = AsString(value);
        } else if (column == "used_tokens") {
            snapshot.used_tokens = AsInt64(value);
        } else if (column == "max_tokens") {
            snapshot.max_tokens = AsInt64(value);
        } else if (column == "included_item_ids") {
            snapshot.included_item_ids = SplitStringList(AsString(value));
        } else if (column == "included_item_source_kinds") {
            for (const auto& kind_text : SplitStringList(AsString(value))) {
                snapshot.included_item_source_kinds.push_back(ContextSourceKindFromString(kind_text));
            }
        } else if (column == "created_at") {
            snapshot.created_at = AsInt64(value);
        }
    }
    return snapshot;
}

// Reports whether context_snapshots already has included_item_source_kinds,
// via PRAGMA table_info() -- its row shape is a documented SQLite contract,
// unlike the wording of any particular error message.
Result<bool> HasIncludedItemSourceKindsColumn(Database& database) {
    const auto columns_result = database.Query("PRAGMA table_info(context_snapshots);");
    if (!columns_result) {
        return Result<bool>::Fail(columns_result.Err());
    }
    for (const auto& row : columns_result.Value()) {
        for (const auto& [column, value] : row) {
            if (column == "name" && AsString(value) == "included_item_source_kinds") {
                return Result<bool>::Ok(true);
            }
        }
    }
    return Result<bool>::Ok(false);
}

// Adds included_item_source_kinds to a context_snapshots table created by
// an older EnsureSchema() that predates this column -- CREATE TABLE IF
// NOT EXISTS alone only shapes a brand-new table, it does nothing to one
// that already exists (docs/ROADMAP.md "Context Restore" -- Symbol/
// Dependency/Keyword restoration needs to know each id's original
// ContextSourceKind, which the id string alone can't safely reveal, see
// ContextSnapshot.hpp's own comment on the field). Checked via
// PRAGMA table_info() rather than trying ALTER TABLE and swallowing a
// "duplicate column" error string -- SQLite's wording for that error
// isn't part of any documented stability contract, while table_info's
// row shape is. A fresh CREATE TABLE (the branch above) already includes
// the column, so this is a no-op for a database that never existed
// before this change -- it only ever does real work migrating an OLD one.
//
// EnsureSchema() runs unsynchronized against a database file that this
// codebase's own architecture has multiple OS processes (the GUI and a
// separately-launched aistudio_core_cli in HTTP mode) open connections to
// concurrently: the check-then-act between the PRAGMA above and the
// ALTER TABLE below is racy across processes -- there's no cross-process
// lock held between them. If two processes both observe the column absent
// and both run ALTER TABLE, SQLite lets exactly one of them add the
// column; the loser's ALTER TABLE fails with "duplicate column name". That
// failure means the migration's *goal* -- the column existing -- was
// already achieved by the winner, so it's benign. Rather than string-match
// the error (same reasoning as the PRAGMA choice above: SQLite's message
// text is not a stability contract), re-run the PRAGMA check after a
// failed ALTER TABLE: if the column exists now, some other process won
// the race and this call is treated as a no-op success; if it still
// doesn't, the ALTER TABLE failure was real (e.g. disk full, locked file)
// and is propagated as before.
Result<void> EnsureIncludedItemSourceKindsColumn(Database& database) {
    const auto has_column_result = HasIncludedItemSourceKindsColumn(database);
    if (!has_column_result) {
        return Result<void>::Fail(has_column_result.Err());
    }
    if (has_column_result.Value()) {
        return Result<void>::Ok();
    }

    const auto alter_result = database.Execute(
        "ALTER TABLE context_snapshots ADD COLUMN included_item_source_kinds TEXT NOT NULL DEFAULT '';");
    if (alter_result) {
        return alter_result;
    }

    // ALTER TABLE failed -- before propagating, check whether another
    // process racing us already added the column. If so, this is not a
    // real failure: the schema this call exists to guarantee is in place.
    const auto recheck_result = HasIncludedItemSourceKindsColumn(database);
    if (recheck_result && recheck_result.Value()) {
        return Result<void>::Ok();
    }
    return alter_result;
}

} // namespace

Result<void> ContextSnapshotRepository::EnsureSchema() {
    if (const auto create_result = database_.Execute(
            "CREATE TABLE IF NOT EXISTS context_snapshots ("
            "  id TEXT PRIMARY KEY,"
            "  task_name TEXT NOT NULL,"
            "  git_commit TEXT NOT NULL DEFAULT '',"
            "  used_tokens INTEGER NOT NULL,"
            "  max_tokens INTEGER NOT NULL,"
            "  included_item_ids TEXT NOT NULL DEFAULT '',"
            "  included_item_source_kinds TEXT NOT NULL DEFAULT '',"
            "  created_at INTEGER NOT NULL"
            ");");
        !create_result) {
        return create_result;
    }
    return EnsureIncludedItemSourceKindsColumn(database_);
}

Result<void> ContextSnapshotRepository::Save(const ContextSnapshot& snapshot) {
    std::vector<std::string> kind_strings;
    kind_strings.reserve(snapshot.included_item_source_kinds.size());
    for (const auto kind : snapshot.included_item_source_kinds) {
        kind_strings.push_back(ToString(kind));
    }
    return database_.Execute(
        "INSERT INTO context_snapshots "
        "  (id, task_name, git_commit, used_tokens, max_tokens, included_item_ids, "
        "   included_item_source_kinds, created_at) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  task_name = excluded.task_name, git_commit = excluded.git_commit, "
        "  used_tokens = excluded.used_tokens, max_tokens = excluded.max_tokens, "
        "  included_item_ids = excluded.included_item_ids, "
        "  included_item_source_kinds = excluded.included_item_source_kinds, "
        "  created_at = excluded.created_at;",
        {
            snapshot.id,
            snapshot.task_name,
            snapshot.git_commit,
            snapshot.used_tokens,
            snapshot.max_tokens,
            JoinStringList(snapshot.included_item_ids),
            JoinStringList(kind_strings),
            snapshot.created_at,
        });
}

Result<std::optional<ContextSnapshot>> ContextSnapshotRepository::FindById(const std::string& id) {
    const auto rows_result = database_.Query("SELECT * FROM context_snapshots WHERE id = ?;", {id});
    if (!rows_result) {
        return Result<std::optional<ContextSnapshot>>::Fail(rows_result.Err());
    }
    const auto& rows = rows_result.Value();
    if (rows.empty()) {
        return Result<std::optional<ContextSnapshot>>::Ok(std::nullopt);
    }
    return Result<std::optional<ContextSnapshot>>::Ok(RowToSnapshot(rows.front()));
}

Result<std::vector<ContextSnapshot>> ContextSnapshotRepository::FindAll() {
    const auto rows_result = database_.Query("SELECT * FROM context_snapshots ORDER BY rowid;");
    if (!rows_result) {
        return Result<std::vector<ContextSnapshot>>::Fail(rows_result.Err());
    }
    std::vector<ContextSnapshot> snapshots;
    snapshots.reserve(rows_result.Value().size());
    for (const auto& row : rows_result.Value()) {
        snapshots.push_back(RowToSnapshot(row));
    }
    return Result<std::vector<ContextSnapshot>>::Ok(std::move(snapshots));
}

Result<void> ContextSnapshotRepository::Remove(const std::string& id) {
    return database_.Execute("DELETE FROM context_snapshots WHERE id = ?;", {id});
}

} // namespace aistudio::core
