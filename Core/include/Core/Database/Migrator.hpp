#pragma once

#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

struct Migration {
    int version;
    std::string name;
    std::string sql;
};

// Applies pending Migrations to a Database in version order, tracked via
// a `schema_migrations` table, so Core's schema can evolve without
// hand-run SQL (docs/ROADMAP.md Phase 1 "Migration system"). Applying the
// same migration list twice is a no-op for already-applied versions.
class Migrator {
public:
    explicit Migrator(Database& database) : database_(database) {}

    Result<void> EnsureMigrationsTable();
    [[nodiscard]] Result<int> CurrentVersion();
    Result<void> Apply(const std::vector<Migration>& migrations);

private:
    Database& database_;
};

} // namespace aistudio::core
