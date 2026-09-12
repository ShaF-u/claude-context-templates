#pragma once

#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Task/Task.hpp"

#include <optional>
#include <vector>

namespace aistudio::core {

// Persists Task to SQLite. A concrete example of the Repository pattern
// Phase 1 calls for ("Repository abstraction"): callers work with the
// Task domain object, never raw SQL — the storage engine can change
// later without touching TaskQueue or Agent code.
//
// EnsureSchema() is a convenience for tests/standalone use. Production
// bootstrap applies the `tasks` table via Migrator instead, so migration
// history — not this method — stays the single source of truth for the
// persisted schema.
class TaskRepository {
public:
    explicit TaskRepository(Database& database) : database_(database) {}

    Result<void> EnsureSchema();
    Result<void> Save(const Task& task);
    [[nodiscard]] Result<std::optional<Task>> FindById(const std::string& id);
    [[nodiscard]] Result<std::vector<Task>> FindAll();
    Result<void> Remove(const std::string& id);

private:
    Database& database_;
};

} // namespace aistudio::core
