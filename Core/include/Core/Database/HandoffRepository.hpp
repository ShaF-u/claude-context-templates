#pragma once

#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Session/HandoffRecord.hpp"

#include <optional>
#include <vector>

namespace aistudio::core {

// Persists HandoffRecord to SQLite (docs/ROADMAP.md 13-6's "共通形式で
// の記録" -- same shape/reasoning as TaskRepository/SessionRepository).
// `verifications` is stored as a single JSON column (same choice
// WorktreeMcpConfig already made for structured data) rather than
// parallel string-list columns -- HandoffVerification has 5 fields per
// entry, and ContextSnapshotRepository's parallel-list approach only
// stays readable for 1-2 fields per entry.
class HandoffRepository {
public:
    explicit HandoffRepository(Database& database) : database_(database) {}

    Result<void> EnsureSchema();
    Result<void> Save(const HandoffRecord& record);
    [[nodiscard]] Result<std::optional<HandoffRecord>> FindById(const std::string& id);
    [[nodiscard]] Result<std::vector<HandoffRecord>> FindAll();
    [[nodiscard]] Result<std::vector<HandoffRecord>> FindByTaskId(const std::string& task_id);
    Result<void> Remove(const std::string& id);

private:
    Database& database_;
};

} // namespace aistudio::core
