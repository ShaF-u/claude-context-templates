#include "test_framework.hpp"
#include "Core/Database/ContextSnapshotRepository.hpp"

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

using namespace aistudio::core;

namespace {
ContextSnapshot MakeSnapshot(std::string id) {
    ContextSnapshot snapshot;
    snapshot.id = std::move(id);
    snapshot.task_name = "Fix Player Attack";
    snapshot.used_tokens = 1982;
    snapshot.max_tokens = 2000;
    snapshot.included_item_ids = {"a.cpp", "b.cpp"};
    snapshot.created_at = 1700000000;
    return snapshot;
}
} // namespace

AISTUDIO_TEST(ContextSnapshotRepository_SaveAndFindById_RoundTrips) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();

    const auto snapshot = MakeSnapshot("snap-1");
    AISTUDIO_EXPECT(repo.Save(snapshot));

    const auto found_result = repo.FindById("snap-1");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.task_name == "Fix Player Attack");
    AISTUDIO_EXPECT(loaded.used_tokens == 1982);
    AISTUDIO_EXPECT(loaded.max_tokens == 2000);
    AISTUDIO_EXPECT(loaded.included_item_ids.size() == 2);
    AISTUDIO_EXPECT(loaded.included_item_ids[0] == "a.cpp");
    AISTUDIO_EXPECT(loaded.created_at == 1700000000);
}

AISTUDIO_TEST(ContextSnapshotRepository_Save_UpsertsExistingId) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();

    auto snapshot = MakeSnapshot("snap-1");
    repo.Save(snapshot);

    snapshot.task_name = "Different Task";
    repo.Save(snapshot);

    const auto all_result = repo.FindAll();
    AISTUDIO_EXPECT(all_result.IsOk());
    AISTUDIO_EXPECT(all_result.Value().size() == 1);
    AISTUDIO_EXPECT(all_result.Value()[0].task_name == "Different Task");
}

AISTUDIO_TEST(ContextSnapshotRepository_FindById_MissingReturnsNullopt) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();

    const auto result = repo.FindById("missing");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(!result.Value().has_value());
}

AISTUDIO_TEST(ContextSnapshotRepository_Remove_DeletesRow) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();

    repo.Save(MakeSnapshot("snap-1"));
    AISTUDIO_EXPECT(repo.Remove("snap-1"));

    const auto result = repo.FindById("snap-1");
    AISTUDIO_EXPECT(!result.Value().has_value());
}

AISTUDIO_TEST(ContextSnapshotRepository_FindAll_EmptyWhenNoSnapshots) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();

    const auto result = repo.FindAll();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().empty());
}

AISTUDIO_TEST(ContextSnapshotRepository_SaveAndFindById_RoundTripsSourceKinds) {
    // docs/ROADMAP.md "Context Restore" -- ContextRestorer needs each
    // id's real ContextSourceKind back, not just the id string.
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();

    auto snapshot = MakeSnapshot("snap-kinds");
    snapshot.included_item_ids = {"a.cpp", "a.cpp:Foo", "a.cpp->b.hpp"};
    snapshot.included_item_source_kinds = {ContextSourceKind::File, ContextSourceKind::Symbol,
                                            ContextSourceKind::Dependency};
    AISTUDIO_EXPECT(repo.Save(snapshot));

    const auto found_result = repo.FindById("snap-kinds");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.included_item_source_kinds.size() == 3);
    AISTUDIO_EXPECT(loaded.included_item_source_kinds[0] == ContextSourceKind::File);
    AISTUDIO_EXPECT(loaded.included_item_source_kinds[1] == ContextSourceKind::Symbol);
    AISTUDIO_EXPECT(loaded.included_item_source_kinds[2] == ContextSourceKind::Dependency);
}

AISTUDIO_TEST(ContextSnapshotRepository_FindById_PreMigrationRow_SourceKindsIsEmpty) {
    // Simulates a database created by an EnsureSchema() from before
    // included_item_source_kinds existed: a hand-built context_snapshots
    // table with only the original columns, exercising EnsureSchema()'s
    // ALTER TABLE migration path (a fresh CREATE TABLE IF NOT EXISTS
    // never hits it, since it already includes the new column).
    Database db;
    db.Open(":memory:");
    AISTUDIO_EXPECT(db.Execute(
        "CREATE TABLE context_snapshots ("
        "  id TEXT PRIMARY KEY,"
        "  task_name TEXT NOT NULL,"
        "  git_commit TEXT NOT NULL DEFAULT '',"
        "  used_tokens INTEGER NOT NULL,"
        "  max_tokens INTEGER NOT NULL,"
        "  included_item_ids TEXT NOT NULL DEFAULT '',"
        "  created_at INTEGER NOT NULL"
        ");"));
    AISTUDIO_EXPECT(db.Execute(
        "INSERT INTO context_snapshots (id, task_name, used_tokens, max_tokens, included_item_ids, created_at) "
        "VALUES ('old-snap', 'Old Task', 5, 100, 'a.cpp', 1700000000);"));

    ContextSnapshotRepository repo(db);
    AISTUDIO_EXPECT(repo.EnsureSchema()); // must migrate the existing table, not just no-op

    const auto found_result = repo.FindById("old-snap");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.included_item_ids.size() == 1);
    AISTUDIO_EXPECT(loaded.included_item_ids[0] == "a.cpp");
    AISTUDIO_EXPECT(loaded.included_item_source_kinds.empty());

    // A second EnsureSchema() call (e.g. a later process startup) must
    // stay idempotent -- ALTER TABLE ADD COLUMN on an already-migrated
    // table would otherwise fail with "duplicate column name".
    AISTUDIO_EXPECT(repo.EnsureSchema());
}

AISTUDIO_TEST(ContextSnapshotRepository_EnsureSchema_ConcurrentMigration_NeitherHardFails) {
    // Reproduces the concurrency bug this branch's schema migration had:
    // EnsureIncludedItemSourceKindsColumn() (see ContextSnapshotRepository.cpp)
    // does an un-transacted check-then-act -- PRAGMA table_info() to see
    // whether included_item_source_kinds already exists, then ALTER TABLE
    // ADD COLUMN if not -- against aistudio.db, a file this codebase's own
    // architecture has multiple separate OS processes (the GUI and a
    // separately-launched aistudio_core_cli in HTTP mode) open their own
    // connection to concurrently. If two processes' EnsureSchema() calls
    // race against a pre-migration database, both PRAGMA checks can see
    // the column absent before either commits its ALTER TABLE, so the
    // loser's ALTER TABLE fails with "duplicate column name" -- which
    // used to be propagated as a hard EnsureSchema() failure, breaking
    // Context Snapshot/Restore for that process until restart.
    //
    // Reproduced with two real OS threads, each opening its own
    // Database connection (:memory: isn't shared across connections, so
    // this uses a real temp file, like the two processes it stands in
    // for) to the same pre-migration database, racing EnsureSchema().
    namespace fs = std::filesystem;
    static std::atomic<int> counter{0};
    const fs::path db_path =
        fs::temp_directory_path() / fs::path("aistudio_snapshot_race_test_" + std::to_string(counter++) + ".db");
    fs::remove(db_path);
    const std::string path_string = db_path.string();

    {
        // Hand-build the pre-migration table shape (matching the
        // PreMigrationRow test above) so both racing EnsureSchema() calls
        // actually have to migrate it -- a fresh CREATE TABLE IF NOT
        // EXISTS already includes the column and would never touch the
        // racy ALTER TABLE path at all.
        Database setup;
        AISTUDIO_EXPECT(setup.Open(path_string));
        AISTUDIO_EXPECT(setup.Execute(
            "CREATE TABLE context_snapshots ("
            "  id TEXT PRIMARY KEY,"
            "  task_name TEXT NOT NULL,"
            "  git_commit TEXT NOT NULL DEFAULT '',"
            "  used_tokens INTEGER NOT NULL,"
            "  max_tokens INTEGER NOT NULL,"
            "  included_item_ids TEXT NOT NULL DEFAULT '',"
            "  created_at INTEGER NOT NULL"
            ");"));
        AISTUDIO_EXPECT(setup.Execute(
            "INSERT INTO context_snapshots (id, task_name, used_tokens, max_tokens, included_item_ids, created_at) "
            "VALUES ('old-snap', 'Old Task', 5, 100, 'a.cpp', 1700000000);"));
    }

    std::atomic<int> ready{0};
    bool ok_a = false;
    bool ok_b = false;
    std::string error_a;
    std::string error_b;

    // Deliberately no try/catch escape via AISTUDIO_EXPECT here: an
    // exception thrown on a worker thread and never caught on that same
    // thread would call std::terminate rather than fail the test, so
    // failures are captured into ok_/error_ instead and asserted after
    // both threads are joined back onto the test thread below.
    auto race = [&](bool& ok_out, std::string& error_out) {
        Database db;
        if (!db.Open(path_string)) {
            ok_out = false;
            error_out = "failed to open " + path_string;
            return;
        }
        ContextSnapshotRepository repo(db);
        ++ready;
        while (ready.load() < 2) {
            // Spin until both connections are open and about to call
            // EnsureSchema(), to maximize the chance both threads'
            // PRAGMA table_info() checks race each other.
        }
        const auto result = repo.EnsureSchema();
        ok_out = static_cast<bool>(result);
        if (!ok_out) {
            error_out = result.Err().ToString();
        }
    };

    std::thread thread_a([&] { race(ok_a, error_a); });
    std::thread thread_b([&] { race(ok_b, error_b); });
    thread_a.join();
    thread_b.join();

    AISTUDIO_EXPECT(ok_a); // must not hard-fail even when it loses the ALTER TABLE race
    AISTUDIO_EXPECT(ok_b);

    // Confirm the race didn't leave the schema corrupted or duplicated:
    // exactly one included_item_source_kinds column, and the pre-existing
    // row plus a fresh write through the migrated column both round-trip.
    //
    // Scoped so `verify`'s SQLite connection is closed (destructor runs)
    // before fs::remove() below -- unlike POSIX, Windows will not delete
    // a file that a still-open handle (this connection) has open.
    {
        Database verify;
        AISTUDIO_EXPECT(verify.Open(path_string));
        const auto columns_result = verify.Query("PRAGMA table_info(context_snapshots);");
        AISTUDIO_EXPECT(columns_result.IsOk());
        int matching_columns = 0;
        for (const auto& row : columns_result.Value()) {
            for (const auto& [column, value] : row) {
                if (column == "name" && AsString(value) == "included_item_source_kinds") {
                    ++matching_columns;
                }
            }
        }
        AISTUDIO_EXPECT(matching_columns == 1);

        ContextSnapshotRepository verify_repo(verify);
        const auto found_result = verify_repo.FindById("old-snap");
        AISTUDIO_EXPECT(found_result.IsOk());
        AISTUDIO_EXPECT(found_result.Value().has_value());
        AISTUDIO_EXPECT(found_result.Value()->included_item_ids.size() == 1);
        AISTUDIO_EXPECT(found_result.Value()->included_item_source_kinds.empty());

        auto new_snapshot = MakeSnapshot("race-snap");
        new_snapshot.included_item_source_kinds = {ContextSourceKind::File};
        AISTUDIO_EXPECT(verify_repo.Save(new_snapshot));
        const auto new_found_result = verify_repo.FindById("race-snap");
        AISTUDIO_EXPECT(new_found_result.IsOk());
        AISTUDIO_EXPECT(new_found_result.Value().has_value());
        AISTUDIO_EXPECT(new_found_result.Value()->included_item_source_kinds.size() == 1);
    }

    fs::remove(db_path);
}
