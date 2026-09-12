#include "test_framework.hpp"
#include "Core/Context/ContextUsageSummary.hpp"

using namespace aistudio::core;

namespace {

ContextAuditEntry MakeEntry(std::string item_id, ContextAuditAction action, std::string detail,
                             std::int64_t timestamp = 1700000000) {
    ContextAuditEntry entry;
    entry.id = "audit-" + item_id;
    entry.item_id = std::move(item_id);
    entry.action = action;
    entry.detail = std::move(detail);
    entry.timestamp = timestamp;
    return entry;
}

} // namespace

AISTUDIO_TEST(SummarizeContextUsage_EmptyEntries_ReturnsEmptySummary) {
    const auto summary = SummarizeContextUsage({});
    AISTUDIO_EXPECT(summary.included.empty());
    AISTUDIO_EXPECT(summary.excluded.empty());
    AISTUDIO_EXPECT(summary.compressed.empty());
    AISTUDIO_EXPECT(summary.total_included_tokens == 0);
}

AISTUDIO_TEST(SummarizeContextUsage_IncludedEntry_LandsInIncludedBucket) {
    const auto summary = SummarizeContextUsage({MakeEntry("a.cpp", ContextAuditAction::Included, "tokens=120")});
    AISTUDIO_EXPECT(summary.included.size() == 1);
    AISTUDIO_EXPECT(summary.included[0].item_id == "a.cpp");
    AISTUDIO_EXPECT(summary.included[0].estimated_tokens == 120);
    AISTUDIO_EXPECT(summary.total_included_tokens == 120);
}

AISTUDIO_TEST(SummarizeContextUsage_ExcludedEntry_LandsInExcludedBucket_NotCountedInTotal) {
    const auto summary = SummarizeContextUsage({MakeEntry("b.cpp", ContextAuditAction::Excluded, "tokens=500")});
    AISTUDIO_EXPECT(summary.excluded.size() == 1);
    AISTUDIO_EXPECT(summary.excluded[0].estimated_tokens == 500);
    AISTUDIO_EXPECT(summary.total_included_tokens == 0);
}

AISTUDIO_TEST(SummarizeContextUsage_CompressedEntry_UsesOriginalSizeAsEstimatedTokens) {
    const auto summary =
        SummarizeContextUsage({MakeEntry("c.cpp", ContextAuditAction::Compressed, "tokens=1000->200")});
    AISTUDIO_EXPECT(summary.compressed.size() == 1);
    AISTUDIO_EXPECT(summary.compressed[0].estimated_tokens == 1000);
}

AISTUDIO_TEST(SummarizeContextUsage_CompressedEntry_CountsPostCompressionSizeInTotal) {
    const auto summary =
        SummarizeContextUsage({MakeEntry("c.cpp", ContextAuditAction::Compressed, "tokens=1000->200")});
    AISTUDIO_EXPECT(summary.total_included_tokens == 200);
}

AISTUDIO_TEST(SummarizeContextUsage_MalformedDetail_IsSkipped) {
    const auto summary = SummarizeContextUsage({MakeEntry("d.cpp", ContextAuditAction::Included, "not-parseable")});
    AISTUDIO_EXPECT(summary.included.empty());
    AISTUDIO_EXPECT(summary.total_included_tokens == 0);
}

AISTUDIO_TEST(SummarizeContextUsage_MixedEntries_SumsIncludedAndCompressedIntoTotal) {
    const auto summary = SummarizeContextUsage({
        MakeEntry("a.cpp", ContextAuditAction::Included, "tokens=100"),
        MakeEntry("b.cpp", ContextAuditAction::Excluded, "tokens=9999"),
        MakeEntry("c.cpp", ContextAuditAction::Compressed, "tokens=1000->200"),
    });
    AISTUDIO_EXPECT(summary.included.size() == 1);
    AISTUDIO_EXPECT(summary.excluded.size() == 1);
    AISTUDIO_EXPECT(summary.compressed.size() == 1);
    AISTUDIO_EXPECT(summary.total_included_tokens == 300); // 100 (included) + 200 (compressed final size)
}

AISTUDIO_TEST(SummarizeContextUsage_PreservesTimestamp) {
    const auto summary =
        SummarizeContextUsage({MakeEntry("a.cpp", ContextAuditAction::Included, "tokens=1", 1712345678)});
    AISTUDIO_EXPECT(summary.included[0].timestamp == 1712345678);
}

AISTUDIO_TEST(SummarizeContextUsage_LegacyDetailWithoutPriority_DefaultsPriorityToMinusOne) {
    const auto summary = SummarizeContextUsage({MakeEntry("a.cpp", ContextAuditAction::Included, "tokens=100")});
    AISTUDIO_EXPECT(summary.included[0].priority == -1);
    AISTUDIO_EXPECT(summary.included[0].estimated_tokens == 100);
}

AISTUDIO_TEST(SummarizeContextUsage_DetailWithPriorityPrefix_ParsesPriorityAndTokens) {
    const auto summary =
        SummarizeContextUsage({MakeEntry("a.cpp", ContextAuditAction::Included, "priority=75 tokens=100")});
    AISTUDIO_EXPECT(summary.included[0].priority == 75);
    AISTUDIO_EXPECT(summary.included[0].estimated_tokens == 100);
}

AISTUDIO_TEST(SummarizeContextUsage_CompressedDetailWithPriorityPrefix_ParsesAllThree) {
    const auto summary =
        SummarizeContextUsage({MakeEntry("c.cpp", ContextAuditAction::Compressed, "priority=90 tokens=1000->200")});
    AISTUDIO_EXPECT(summary.compressed[0].priority == 90);
    AISTUDIO_EXPECT(summary.compressed[0].estimated_tokens == 1000);
    AISTUDIO_EXPECT(summary.total_included_tokens == 200);
}
