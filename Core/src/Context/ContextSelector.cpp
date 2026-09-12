#include "Core/Context/ContextSelector.hpp"

#include "Core/Context/ContextAuditEntry.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Protocol/RequestId.hpp"
#include "Core/Util/Time.hpp"

#include <algorithm>

namespace aistudio::core {

namespace {

void SortByPriorityDescending(std::vector<ContextItem>& items) {
    std::stable_sort(items.begin(), items.end(),
                      [](const ContextItem& a, const ContextItem& b) { return a.priority > b.priority; });
}

// Publishes one audit entry per decision (docs/MASTER_SPEC.md #19). A
// listener persists these via ContextAuditRepository if it wants to;
// ContextSelector itself doesn't know or care whether anyone is
// listening (AGENT.md #5).
void PublishAudit(const std::string& item_id, ContextAuditAction action, const std::string& detail) {
    ContextAuditEntry entry;
    entry.id = RequestIdGenerator::Next();
    entry.item_id = item_id;
    entry.action = action;
    entry.timestamp = CurrentUnixTimestamp();
    entry.detail = detail;
    EventBus::Instance().Publish("ContextAudit", entry);
}

// "priority=N " ahead of the existing "tokens=..." detail --
// SummarizeContextUsage()'s ContextUsageItem::priority parses this back
// out (docs/ROADMAP.md Context UI "Priority display").
std::string TokensDetail(int priority, const std::string& tokens_part) {
    return "priority=" + std::to_string(priority) + " tokens=" + tokens_part;
}

} // namespace

const ContextItemCost& DefaultContextItemCost() {
    static const ContextItemCost cost = [](const ContextItem& item) { return item.estimated_tokens; };
    return cost;
}

ContextSelection ContextSelector::Select(std::vector<ContextItem> candidates, ContextBudget budget) const {
    return Select(std::move(candidates), budget, DefaultContextItemCost());
}

ContextSelection ContextSelector::SelectWithCompression(std::vector<ContextItem> candidates, ContextBudget budget,
                                                          const ContextCompressor& compressor) const {
    return SelectWithCompression(std::move(candidates), budget, compressor, DefaultContextItemCost());
}

ContextSelection ContextSelector::Select(std::vector<ContextItem> candidates, ContextBudget budget,
                                          const ContextItemCost& cost) const {
    SortByPriorityDescending(candidates);

    ContextSelection selection;
    selection.included.reserve(candidates.size());

    for (auto& item : candidates) {
        const auto id = item.id;
        const auto priority = item.priority;
        const auto tokens = cost(item);
        if (budget.Charge(tokens)) {
            selection.included.push_back(std::move(item));
            PublishAudit(id, ContextAuditAction::Included, TokensDetail(priority, std::to_string(tokens)));
        } else {
            selection.excluded.push_back(std::move(item));
            PublishAudit(id, ContextAuditAction::Excluded, TokensDetail(priority, std::to_string(tokens)));
        }
    }

    selection.used_tokens = budget.UsedTokens();
    return selection;
}

ContextSelection ContextSelector::SelectWithCompression(std::vector<ContextItem> candidates, ContextBudget budget,
                                                          const ContextCompressor& compressor,
                                                          const ContextItemCost& cost) const {
    SortByPriorityDescending(candidates);

    ContextSelection selection;
    selection.included.reserve(candidates.size());

    for (auto& item : candidates) {
        const auto id = item.id;
        const auto priority = item.priority;
        const auto tokens = cost(item);
        if (budget.Charge(tokens)) {
            selection.included.push_back(std::move(item));
            PublishAudit(id, ContextAuditAction::Included, TokensDetail(priority, std::to_string(tokens)));
            continue;
        }

        const auto remaining = budget.RemainingTokens();
        // 0 for the default cost model.
        const auto overhead = tokens - item.estimated_tokens;
        const auto content_target = remaining - overhead;
        if (remaining > 0 && content_target > 0) {
            auto compressed = compressor.Compress(item, content_target);
            const auto compressed_tokens = cost(compressed);
            if (compressed_tokens <= remaining && budget.Charge(compressed_tokens)) {
                selection.included.push_back(std::move(compressed));
                PublishAudit(id, ContextAuditAction::Compressed,
                             TokensDetail(priority, std::to_string(tokens) + "->" + std::to_string(compressed_tokens)));
                continue;
            }
        }

        selection.excluded.push_back(std::move(item));
        PublishAudit(id, ContextAuditAction::Excluded, TokensDetail(priority, std::to_string(tokens)));
    }

    selection.used_tokens = budget.UsedTokens();
    return selection;
}

} // namespace aistudio::core
