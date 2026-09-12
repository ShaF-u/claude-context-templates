#pragma once

#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Session/OperationLedger.hpp"

#include <optional>
#include <vector>

namespace aistudio::core {

// Persists OperationRecord to SQLite (docs/ROADMAP.md 13-7's "操作の識
// 別子・開始/完了状態・成果物を保存する" -- so the idempotency ledger
// survives the process restart it exists to guard against; an in-memory
// OperationLedger alone would lose exactly the information needed the
// moment the process that crashed mid-operation is the one being
// recovered from). Same shape as TaskRepository/SessionRepository/
// HandoffRepository.
class OperationLedgerRepository {
public:
    explicit OperationLedgerRepository(Database& database) : database_(database) {}

    Result<void> EnsureSchema();
    Result<void> Save(const OperationRecord& record);
    [[nodiscard]] Result<std::optional<OperationRecord>> FindById(const std::string& id);
    [[nodiscard]] Result<std::vector<OperationRecord>> FindAll();
    Result<void> Remove(const std::string& id);

private:
    Database& database_;
};

} // namespace aistudio::core
