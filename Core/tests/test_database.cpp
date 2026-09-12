#include "test_framework.hpp"
#include "Core/Database/Database.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace aistudio::core;

AISTUDIO_TEST(Database_Open_InMemory_Succeeds) {
    Database db;
    AISTUDIO_EXPECT(db.Open(":memory:"));
    AISTUDIO_EXPECT(db.IsOpen());
}

AISTUDIO_TEST(Database_Execute_CreateTableAndInsert) {
    Database db;
    db.Open(":memory:");
    AISTUDIO_EXPECT(db.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);"));
    AISTUDIO_EXPECT(db.Execute("INSERT INTO t (id, name) VALUES (?, ?);", {std::int64_t{1}, std::string("alice")}));
}

AISTUDIO_TEST(Database_Query_ReturnsBoundValues) {
    Database db;
    db.Open(":memory:");
    db.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);");
    db.Execute("INSERT INTO t (id, name) VALUES (?, ?);", {std::int64_t{1}, std::string("alice")});

    const auto rows_result = db.Query("SELECT id, name FROM t WHERE id = ?;", {std::int64_t{1}});
    AISTUDIO_EXPECT(rows_result.IsOk());
    const auto& rows = rows_result.Value();
    AISTUDIO_EXPECT(rows.size() == 1);
    AISTUDIO_EXPECT(AsString(rows[0][1].second) == "alice");
}

AISTUDIO_TEST(Database_Parameters_PreventInjection) {
    Database db;
    db.Open(":memory:");
    db.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);");
    // Bound as a parameter, not concatenated into the statement text, so
    // this must be stored literally rather than executed as SQL.
    const std::string malicious = "'); DROP TABLE t; --";
    AISTUDIO_EXPECT(db.Execute("INSERT INTO t (id, name) VALUES (?, ?);", {std::int64_t{1}, malicious}));

    const auto rows_result = db.Query("SELECT name FROM t;");
    AISTUDIO_EXPECT(rows_result.IsOk());
    AISTUDIO_EXPECT(rows_result.Value().size() == 1);
    AISTUDIO_EXPECT(AsString(rows_result.Value()[0][0].second) == malicious);
}

AISTUDIO_TEST(Database_Transaction_RollbackDiscardsChanges) {
    Database db;
    db.Open(":memory:");
    db.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY);");

    AISTUDIO_EXPECT(db.Begin());
    db.Execute("INSERT INTO t (id) VALUES (1);");
    AISTUDIO_EXPECT(db.Rollback());

    const auto rows_result = db.Query("SELECT * FROM t;");
    AISTUDIO_EXPECT(rows_result.Value().empty());
}

AISTUDIO_TEST(Database_Transaction_CommitPersistsChanges) {
    Database db;
    db.Open(":memory:");
    db.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY);");

    AISTUDIO_EXPECT(db.Begin());
    db.Execute("INSERT INTO t (id) VALUES (1);");
    AISTUDIO_EXPECT(db.Commit());

    const auto rows_result = db.Query("SELECT * FROM t;");
    AISTUDIO_EXPECT(rows_result.Value().size() == 1);
}

AISTUDIO_TEST(Database_BackupTo_CopiesData) {
    Database source;
    source.Open(":memory:");
    source.Execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);");
    source.Execute("INSERT INTO t (id, name) VALUES (?, ?);", {std::int64_t{1}, std::string("bob")});

    const std::string backup_path = "test_backup_output.sqlite3";
    std::remove(backup_path.c_str());
    AISTUDIO_EXPECT(source.BackupTo(backup_path));

    Database restored;
    AISTUDIO_EXPECT(restored.Open(backup_path));
    const auto rows_result = restored.Query("SELECT name FROM t WHERE id = ?;", {std::int64_t{1}});
    AISTUDIO_EXPECT(rows_result.IsOk());
    AISTUDIO_EXPECT(rows_result.Value().size() == 1);
    AISTUDIO_EXPECT(AsString(rows_result.Value()[0][0].second) == "bob");

    restored.Close();
    std::remove(backup_path.c_str());
}
