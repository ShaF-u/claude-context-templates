#pragma once

#include "Core/Context/ContextSnapshot.hpp"
#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"

#include <optional>
#include <vector>

namespace aistudio::core {

// Persists ContextSnapshot to SQLite — the same Repository pattern
// TaskRepository established (docs/ROADMAP.md Phase 2 "Context Snapshot").
class ContextSnapshotRepository {
public:
    explicit ContextSnapshotRepository(Database& database) : database_(database) {}

    Result<void> EnsureSchema();
    Result<void> Save(const ContextSnapshot& snapshot);
    [[nodiscard]] Result<std::optional<ContextSnapshot>> FindById(const std::string& id);
    [[nodiscard]] Result<std::vector<ContextSnapshot>> FindAll();
    Result<void> Remove(const std::string& id);

private:
    Database& database_;
};

} // namespace aistudio::core
