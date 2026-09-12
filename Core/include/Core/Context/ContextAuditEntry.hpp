#pragma once

#include <cstdint>
#include <string>

namespace aistudio::core {

// What ContextSelector did with a ContextItem — docs/MASTER_SPEC.md #19
// Context Audit: "AIが何を読んだかすべて記録する". Published via EventBus
// under "ContextAudit" so persistence stays decoupled from the selection
// logic itself (AGENT.md #5 — state changes should be Events, not direct
// calls into a specific storage backend).
enum class ContextAuditAction {
    Included,
    Excluded,
    Compressed,
};

struct ContextAuditEntry {
    std::string id;      // unique per entry, see RequestIdGenerator
    std::string item_id; // the ContextItem's id
    ContextAuditAction action = ContextAuditAction::Included;
    std::int64_t timestamp = 0; // unix seconds
    std::string detail;         // e.g. "tokens=120"
};

[[nodiscard]] std::string ToString(ContextAuditAction action);

} // namespace aistudio::core
