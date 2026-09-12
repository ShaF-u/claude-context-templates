#include "test_framework.hpp"
#include "Core/Context/ContextAuditEntry.hpp"
#include "Core/Context/ContextCompressor.hpp"
#include "Core/Context/ContextSelector.hpp"
#include "Core/Event/EventBus.hpp"

#include <algorithm>
#include <any>

using namespace aistudio::core;

namespace {
ContextItem MakeItem(std::string id, int priority, std::int64_t tokens) {
    ContextItem item;
    item.id = std::move(id);
    item.priority = priority;
    item.estimated_tokens = tokens;
    return item;
}

ContextItem MakeContentItem(std::string id, int priority, std::string content) {
    ContextItem item;
    item.id = std::move(id);
    item.priority = priority;
    item.content = std::move(content);
    item.estimated_tokens = EstimateTokens(item.content);
    return item;
}

bool Includes(const ContextSelection& selection, const std::string& id) {
    return std::any_of(selection.included.begin(), selection.included.end(),
                        [&](const ContextItem& i) { return i.id == id; });
}

bool Excludes(const ContextSelection& selection, const std::string& id) {
    return std::any_of(selection.excluded.begin(), selection.excluded.end(),
                        [&](const ContextItem& i) { return i.id == id; });
}
} // namespace

AISTUDIO_TEST(ContextSelector_Select_PrefersHigherPriority) {
    ContextSelector selector;
    std::vector<ContextItem> candidates = {
        MakeItem("low", 10, 50),
        MakeItem("high", 90, 50),
    };

    const auto selection = selector.Select(candidates, ContextBudget(50));

    AISTUDIO_EXPECT(Includes(selection, "high"));
    AISTUDIO_EXPECT(Excludes(selection, "low"));
}

AISTUDIO_TEST(ContextSelector_Select_FitsMultipleWithinBudget) {
    ContextSelector selector;
    std::vector<ContextItem> candidates = {
        MakeItem("a", 90, 30),
        MakeItem("b", 80, 30),
        MakeItem("c", 70, 30),
    };

    const auto selection = selector.Select(candidates, ContextBudget(65));

    AISTUDIO_EXPECT(Includes(selection, "a"));
    AISTUDIO_EXPECT(Includes(selection, "b"));
    AISTUDIO_EXPECT(Excludes(selection, "c"));
    AISTUDIO_EXPECT(selection.used_tokens == 60);
}

AISTUDIO_TEST(ContextSelector_Select_SkipsOversizedItem_ButStillFitsSmallerLowerPriorityOne) {
    ContextSelector selector;
    std::vector<ContextItem> candidates = {
        MakeItem("too_big", 90, 1000),
        MakeItem("fits", 10, 20),
    };

    const auto selection = selector.Select(candidates, ContextBudget(50));

    AISTUDIO_EXPECT(Excludes(selection, "too_big"));
    AISTUDIO_EXPECT(Includes(selection, "fits"));
}

AISTUDIO_TEST(ContextSelector_Select_TiesPreserveInputOrder) {
    ContextSelector selector;
    std::vector<ContextItem> candidates = {
        MakeItem("first", 50, 10),
        MakeItem("second", 50, 10),
    };

    const auto selection = selector.Select(candidates, ContextBudget(10));

    AISTUDIO_EXPECT(Includes(selection, "first"));
    AISTUDIO_EXPECT(Excludes(selection, "second"));
}

AISTUDIO_TEST(ContextSelector_Select_EmptyCandidates_ReturnsEmptySelection) {
    ContextSelector selector;
    const auto selection = selector.Select({}, ContextBudget(100));

    AISTUDIO_EXPECT(selection.included.empty());
    AISTUDIO_EXPECT(selection.excluded.empty());
    AISTUDIO_EXPECT(selection.used_tokens == 0);
}

AISTUDIO_TEST(ContextSelector_SelectWithCompression_CompressesOversizedItemToFit) {
    ContextSelector selector;
    ContextCompressor compressor;

    const std::string big_content = "HEAD_" + std::string(2000, 'x') + "_TAIL";
    std::vector<ContextItem> candidates = {MakeContentItem("big", 90, big_content)};

    const auto selection = selector.SelectWithCompression(candidates, ContextBudget(100), compressor);

    AISTUDIO_EXPECT(Includes(selection, "big"));
    AISTUDIO_EXPECT(selection.included.front().compression == CompressionLevel::Summary);
    AISTUDIO_EXPECT(selection.used_tokens <= 100);
}

AISTUDIO_TEST(ContextSelector_SelectWithCompression_StillExcludesWhenBudgetTooSmallToCompressInto) {
    ContextSelector selector;
    ContextCompressor compressor;

    const std::string big_content = "HEAD_" + std::string(2000, 'x') + "_TAIL";
    std::vector<ContextItem> candidates = {MakeContentItem("big", 90, big_content)};

    const auto selection = selector.SelectWithCompression(candidates, ContextBudget(2), compressor);

    AISTUDIO_EXPECT(Excludes(selection, "big"));
}

AISTUDIO_TEST(ContextSelector_SelectWithCompression_ItemThatFitsRaw_IsNotCompressed) {
    ContextSelector selector;
    ContextCompressor compressor;

    std::vector<ContextItem> candidates = {MakeContentItem("small", 90, "short content")};

    const auto selection = selector.SelectWithCompression(candidates, ContextBudget(100), compressor);

    AISTUDIO_EXPECT(Includes(selection, "small"));
    AISTUDIO_EXPECT(selection.included.front().compression == CompressionLevel::Raw);
    AISTUDIO_EXPECT(selection.included.front().content == "short content");
}

AISTUDIO_TEST(ContextSelector_Select_PublishesAuditEventPerDecision) {
    std::vector<ContextAuditEntry> received;
    const auto subscription_id = EventBus::Instance().Subscribe("ContextAudit", [&](const std::any& payload) {
        if (const auto* entry = std::any_cast<ContextAuditEntry>(&payload)) {
            received.push_back(*entry);
        }
    });

    ContextSelector selector;
    std::vector<ContextItem> candidates = {
        MakeItem("kept", 90, 10),
        MakeItem("dropped", 10, 1000),
    };
    const auto selection = selector.Select(candidates, ContextBudget(50));
    AISTUDIO_EXPECT(selection.included.size() == 1);

    EventBus::Instance().Unsubscribe("ContextAudit", subscription_id);

    AISTUDIO_EXPECT(received.size() == 2);
    const auto kept_it =
        std::find_if(received.begin(), received.end(), [](const ContextAuditEntry& e) { return e.item_id == "kept"; });
    const auto dropped_it = std::find_if(received.begin(), received.end(),
                                          [](const ContextAuditEntry& e) { return e.item_id == "dropped"; });
    AISTUDIO_EXPECT(kept_it != received.end() && kept_it->action == ContextAuditAction::Included);
    AISTUDIO_EXPECT(dropped_it != received.end() && dropped_it->action == ContextAuditAction::Excluded);
}

AISTUDIO_TEST(ContextSelector_Select_AuditDetail_IncludesItemPriorityAheadOfTokens) {
    std::vector<ContextAuditEntry> received;
    const auto subscription_id = EventBus::Instance().Subscribe("ContextAudit", [&](const std::any& payload) {
        if (const auto* entry = std::any_cast<ContextAuditEntry>(&payload)) {
            received.push_back(*entry);
        }
    });

    ContextSelector selector;
    std::vector<ContextItem> candidates = {MakeItem("kept", 77, 10)};
    const auto selection = selector.Select(candidates, ContextBudget(50));
    AISTUDIO_EXPECT(selection.included.size() == 1);

    EventBus::Instance().Unsubscribe("ContextAudit", subscription_id);

    AISTUDIO_EXPECT(received.size() == 1);
    AISTUDIO_EXPECT(received[0].detail == "priority=77 tokens=10");
}

// --- Cost model: McpServer charges the serialized entry, not content ---

namespace {
ContextItemCost EnvelopeCost(std::int64_t overhead) {
    return [overhead](const ContextItem& item) { return item.estimated_tokens + overhead; };
}
} // namespace

AISTUDIO_TEST(ContextSelector_Select_CustomCost_ChargesCostNotEstimatedTokens) {
    const ContextSelector selector;
    std::vector<ContextItem> candidates{MakeItem("a", 90, 40), MakeItem("b", 80, 40)};

    // Both fit under the default cost model...
    const auto without_overhead = selector.Select(candidates, ContextBudget(80));
    AISTUDIO_EXPECT(without_overhead.included.size() == 2);

    // ...but not once each also costs a 10-token envelope.
    const auto with_overhead = selector.Select(candidates, ContextBudget(80), EnvelopeCost(10));
    AISTUDIO_EXPECT(with_overhead.included.size() == 1);
    AISTUDIO_EXPECT(Includes(with_overhead, "a"));
    AISTUDIO_EXPECT(Excludes(with_overhead, "b"));
    AISTUDIO_EXPECT(with_overhead.used_tokens == 50);
}

AISTUDIO_TEST(ContextSelector_Select_DefaultCost_MatchesExplicitEstimatedTokensCost) {
    const ContextSelector selector;
    std::vector<ContextItem> candidates{MakeItem("a", 90, 40), MakeItem("b", 80, 40)};

    const auto implicit_cost = selector.Select(candidates, ContextBudget(60));
    const auto explicit_cost = selector.Select(candidates, ContextBudget(60), DefaultContextItemCost());

    AISTUDIO_EXPECT(implicit_cost.included.size() == explicit_cost.included.size());
    AISTUDIO_EXPECT(implicit_cost.used_tokens == explicit_cost.used_tokens);
}

AISTUDIO_TEST(ContextSelector_SelectWithCompression_CustomCost_CompressedItemStillFitsWithItsOverhead) {
    // The compressor sizes content; the envelope is paid on top, so the
    // selector subtracts the overhead before compressing.
    const ContextSelector selector;
    const ContextCompressor compressor;
    const std::int64_t overhead = 10;
    const auto cost = EnvelopeCost(overhead);

    std::vector<ContextItem> candidates{
        MakeItem("small", 90, 30),
        MakeContentItem("large", 50, std::string(4000, 'x')),
    };

    const auto selection = selector.SelectWithCompression(candidates, ContextBudget(120), compressor, cost);

    AISTUDIO_EXPECT(Includes(selection, "small"));
    AISTUDIO_EXPECT(Includes(selection, "large")); // compressed in, not excluded
    std::int64_t total = 0;
    for (const auto& item : selection.included) {
        total += cost(item);
    }
    AISTUDIO_EXPECT(total <= 120);
    AISTUDIO_EXPECT(selection.used_tokens <= 120);
}

AISTUDIO_TEST(ContextSelector_SelectWithCompression_CustomCost_OverheadAloneExceedsRemaining_Excludes) {
    // Remaining budget below the envelope: no content size fits, so the
    // item is excluded rather than emitted over budget.
    const ContextSelector selector;
    const ContextCompressor compressor;

    std::vector<ContextItem> candidates{
        MakeItem("first", 90, 45),
        MakeContentItem("second", 50, std::string(4000, 'x')),
    };

    // 60 fits the first (45 + 10) and leaves 5 -- under the envelope.
    const auto selection = selector.SelectWithCompression(candidates, ContextBudget(60), compressor, EnvelopeCost(10));

    AISTUDIO_EXPECT(Includes(selection, "first"));
    AISTUDIO_EXPECT(Excludes(selection, "second"));
    AISTUDIO_EXPECT(selection.used_tokens == 55);
}
