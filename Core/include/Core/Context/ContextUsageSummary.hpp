#pragma once

#include "Core/Context/ContextAuditEntry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aistudio::core {

// One audit entry reshaped for display — docs/ROADMAP.md "Context UI"
// (usage meter / breakdown / viewer / included items / excluded items /
// token estimation).
struct ContextUsageItem {
    std::string item_id;
    ContextAuditAction action = ContextAuditAction::Included;
    // Parsed from ContextAuditEntry::detail's "tokens=" prefix. For a
    // Compressed entry (detail "tokens=N->M"), this is N — the original
    // pre-compression estimate, which is what "how big was this item"
    // display actually wants; the post-compression size M is in
    // ContextUsageSummary::total_included_tokens instead.
    std::int64_t estimated_tokens = 0;
    std::int64_t timestamp = 0;
    // Parsed from ContextAuditEntry::detail's optional "priority=N " prefix
    // (ContextSelector::PublishAudit() writes it ahead of "tokens="). -1
    // when the entry predates this field and carries no prefix -- older
    // rows already persisted in aistudio.db fall back here rather than
    // failing to parse.
    int priority = -1;
};

struct ContextUsageSummary {
    std::vector<ContextUsageItem> included;
    std::vector<ContextUsageItem> excluded;
    std::vector<ContextUsageItem> compressed;
    // Sum of what each Included/Compressed item actually cost after
    // selection (a Compressed item's post-compression size, not its
    // original one) — the "usage meter" total.
    std::int64_t total_included_tokens = 0;
};

// Reshapes a flat audit log (as returned by ContextAuditRepository::
// FindAll()/FindByItemId()) into the three action buckets a Context UI
// would actually render. Entries whose `detail` doesn't match the
// "tokens=N" / "tokens=N->M" format ContextSelector::PublishAudit()
// writes (a parse failure, or some future action this wasn't updated
// for) are skipped rather than guessed at.
//
// Deliberately out of scope for this pass (AGENT.md #14 "最小実装優先"):
// Cost estimation, which needs $-per-token pricing tied to a specific LLM
// provider (on hold, see CLAUDE.md's 2026-09-01 policy correction).
// Priority display is no longer out of scope -- see ContextUsageItem::priority.
[[nodiscard]] ContextUsageSummary SummarizeContextUsage(const std::vector<ContextAuditEntry>& entries);

} // namespace aistudio::core
