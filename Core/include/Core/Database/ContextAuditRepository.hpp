#pragma once

#include "Core/Context/ContextAuditEntry.hpp"
#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"

#include <vector>

namespace aistudio::core {

// Persists ContextAuditEntry to SQLite. Callers typically feed it by
// subscribing to EventBus's "ContextAudit" event (published by
// ContextSelector) rather than calling Save() directly — see
// aistudio_core_cli's bootstrap wiring. Append-only: entries are a log,
// so there is no Remove()/upsert — each entry.id is unique per event.
class ContextAuditRepository {
public:
    explicit ContextAuditRepository(Database& database) : database_(database) {}

    Result<void> EnsureSchema();
    Result<void> Save(const ContextAuditEntry& entry);
    [[nodiscard]] Result<std::vector<ContextAuditEntry>> FindAll();
    [[nodiscard]] Result<std::vector<ContextAuditEntry>> FindByItemId(const std::string& item_id);

private:
    Database& database_;
};

} // namespace aistudio::core
