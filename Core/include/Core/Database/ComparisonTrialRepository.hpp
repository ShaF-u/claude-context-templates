#pragma once

#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Session/ComparisonBoard.hpp"

#include <optional>
#include <vector>

namespace aistudio::core {

// Persists ComparisonTrial to SQLite (docs/ROADMAP.md 13-8's "不採用の
// 試行も履歴と判断根拠を確認できるようにする" -- a rejected trial must
// survive past the in-memory ComparisonBoard's process lifetime, or its
// rationale is lost exactly when it would matter for a later audit).
// Same shape as TaskRepository/SessionRepository/HandoffRepository/
// OperationLedgerRepository.
class ComparisonTrialRepository {
public:
    explicit ComparisonTrialRepository(Database& database) : database_(database) {}

    Result<void> EnsureSchema();
    Result<void> Save(const ComparisonTrial& trial);
    [[nodiscard]] Result<std::optional<ComparisonTrial>> FindById(const std::string& id);
    [[nodiscard]] Result<std::vector<ComparisonTrial>> FindByComparisonId(const std::string& comparison_id);
    [[nodiscard]] Result<std::vector<ComparisonTrial>> FindAll();
    Result<void> Remove(const std::string& id);

private:
    Database& database_;
};

} // namespace aistudio::core
