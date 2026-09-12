#pragma once

#include "Core/Context/ContextBudget.hpp"
#include "Core/Context/ContextCompressor.hpp"
#include "Core/Context/ContextItem.hpp"

#include <functional>
#include <vector>

namespace aistudio::core {

// What one item costs the caller in tokens. A caller that wraps items in
// something bigger (McpServer serializes each as JSON) passes its own
// cost function rather than inflating ContextItem::estimated_tokens, so
// the item's own token count stays true to its content.
using ContextItemCost = std::function<std::int64_t(const ContextItem&)>;

// item.estimated_tokens.
[[nodiscard]] const ContextItemCost& DefaultContextItemCost();

// Result of a Select() call. `excluded` is kept (not discarded) so a
// future Context Viewer (docs/MASTER_SPEC.md #18) can show what was left
// out and why, instead of only ever seeing what made the cut.
struct ContextSelection {
    std::vector<ContextItem> included;
    std::vector<ContextItem> excluded;
    std::int64_t used_tokens = 0;
};

// Ranks candidates by priority (highest first, ties broken by input
// order for determinism) and greedily fits as many as possible into the
// given budget — docs/MASTER_SPEC.md #14 Context Priority + #17 Context
// Budget Manager, combined into the minimal "select what fits" policy
// Phase 2 needs. A later item that doesn't fit is excluded and scanning
// continues (a smaller lower-priority item further down may still fit),
// rather than stopping at the first miss.
//
// Every Included/Excluded/Compressed decision is published to EventBus
// under "ContextAudit" as a ContextAuditEntry (docs/MASTER_SPEC.md #19
// Context Audit) — a listener persists these if it cares to; the
// Selector itself stays storage-agnostic.
class ContextSelector {
public:
    [[nodiscard]] ContextSelection Select(std::vector<ContextItem> candidates, ContextBudget budget) const;

    // Same policy as Select(), but charges `cost` per item instead of its
    // estimated_tokens — see ContextItemCost.
    [[nodiscard]] ContextSelection Select(std::vector<ContextItem> candidates, ContextBudget budget,
                                           const ContextItemCost& cost) const;

    // Same policy as Select(), but when an item doesn't fit as-is, tries
    // compressing it down to the remaining budget (ContextCompressor)
    // before excluding it — completing the Retrieval -> Ranking ->
    // Compression -> Budget Check pipeline docs/MASTER_SPEC.md #8
    // describes.
    [[nodiscard]] ContextSelection SelectWithCompression(std::vector<ContextItem> candidates, ContextBudget budget,
                                                          const ContextCompressor& compressor) const;

    // Under a caller-supplied cost model, the compression target is the
    // remaining budget minus this item's overhead (cost -
    // estimated_tokens): ContextCompressor sizes content, and the
    // overhead is paid on top of it.
    [[nodiscard]] ContextSelection SelectWithCompression(std::vector<ContextItem> candidates, ContextBudget budget,
                                                          const ContextCompressor& compressor,
                                                          const ContextItemCost& cost) const;
};

} // namespace aistudio::core
