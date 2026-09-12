#include "test_framework.hpp"
#include "Core/Database/Migrator.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(Migrator_Apply_RunsInVersionOrder) {
    Database db;
    db.Open(":memory:");
    Migrator migrator(db);

    const std::vector<Migration> migrations = {
        {2, "add_column", "ALTER TABLE t ADD COLUMN extra TEXT;"},
        {1, "create_table", "CREATE TABLE t (id INTEGER PRIMARY KEY);"},
    };
    AISTUDIO_EXPECT(migrator.Apply(migrations));

    const auto version_result = migrator.CurrentVersion();
    AISTUDIO_EXPECT(version_result.IsOk());
    AISTUDIO_EXPECT(version_result.Value() == 2);
}

AISTUDIO_TEST(Migrator_Apply_IsIdempotent) {
    Database db;
    db.Open(":memory:");
    Migrator migrator(db);

    const std::vector<Migration> migrations = {{1, "create_table", "CREATE TABLE t (id INTEGER PRIMARY KEY);"}};
    AISTUDIO_EXPECT(migrator.Apply(migrations));
    // Re-applying must not re-run version 1 (which would fail with
    // "table already exists").
    AISTUDIO_EXPECT(migrator.Apply(migrations));
}

AISTUDIO_TEST(Migrator_CurrentVersion_ZeroWhenNoMigrationsApplied) {
    Database db;
    db.Open(":memory:");
    Migrator migrator(db);
    migrator.EnsureMigrationsTable();

    const auto version_result = migrator.CurrentVersion();
    AISTUDIO_EXPECT(version_result.IsOk());
    AISTUDIO_EXPECT(version_result.Value() == 0);
}
