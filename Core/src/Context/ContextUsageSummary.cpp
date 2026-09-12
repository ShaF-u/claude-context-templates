#include "Core/Context/ContextUsageSummary.hpp"

#include <charconv>
#include <optional>
#include <string_view>

namespace aistudio::core {

namespace {

std::optional<std::int64_t> ParseInt64(std::string_view text) {
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

// Parses ContextSelector::PublishAudit()'s "tokens=N" (Included/Excluded)
// or "tokens=N->M" (Compressed) detail format. Returns {N, M} — M equals
// N when there's no "->" (the common, non-Compressed case).
struct ParsedTokens {
    std::int64_t original = 0;
    std::int64_t final_size = 0;
};

std::optional<ParsedTokens> ParseTokensDetail(const std::string& detail) {
    constexpr std::string_view prefix = "tokens=";
    if (detail.size() <= prefix.size() || detail.compare(0, prefix.size(), prefix) != 0) {
        return std::nullopt;
    }
    const std::string_view rest(detail.data() + prefix.size(), detail.size() - prefix.size());
    const auto arrow_pos = rest.find("->");
    if (arrow_pos == std::string_view::npos) {
        const auto value = ParseInt64(rest);
        if (!value.has_value()) {
            return std::nullopt;
        }
        return ParsedTokens{*value, *value};
    }
    const auto original = ParseInt64(rest.substr(0, arrow_pos));
    const auto final_size = ParseInt64(rest.substr(arrow_pos + 2));
    if (!original.has_value() || !final_size.has_value()) {
        return std::nullopt;
    }
    return ParsedTokens{*original, *final_size};
}

// Strips ContextSelector::PublishAudit()'s optional "priority=N " prefix
// (written ahead of "tokens=..." since Symbol Compression Level). Returns
// {-1, detail unchanged} when the prefix isn't present -- older audit rows
// persisted before this field existed, or a future detail format this
// wasn't updated for -- so the "tokens=" parse below still runs on
// whatever remains either way.
struct StrippedPriority {
    int priority = -1;
    std::string_view rest;
};

StrippedPriority StripPriorityPrefix(const std::string& detail) {
    constexpr std::string_view prefix = "priority=";
    if (detail.size() <= prefix.size() || detail.compare(0, prefix.size(), prefix) != 0) {
        return StrippedPriority{-1, detail};
    }
    const std::string_view after_prefix(detail.data() + prefix.size(), detail.size() - prefix.size());
    const auto space_pos = after_prefix.find(' ');
    if (space_pos == std::string_view::npos) {
        return StrippedPriority{-1, detail};
    }
    const auto value = ParseInt64(after_prefix.substr(0, space_pos));
    if (!value.has_value()) {
        return StrippedPriority{-1, detail};
    }
    return StrippedPriority{static_cast<int>(*value), after_prefix.substr(space_pos + 1)};
}

} // namespace

ContextUsageSummary SummarizeContextUsage(const std::vector<ContextAuditEntry>& entries) {
    ContextUsageSummary summary;
    for (const auto& entry : entries) {
        const auto stripped = StripPriorityPrefix(entry.detail);
        const auto parsed = ParseTokensDetail(std::string(stripped.rest));
        if (!parsed.has_value()) {
            continue;
        }

        ContextUsageItem item;
        item.item_id = entry.item_id;
        item.action = entry.action;
        item.estimated_tokens = parsed->original;
        item.timestamp = entry.timestamp;
        item.priority = stripped.priority;

        switch (entry.action) {
            case ContextAuditAction::Included:
                summary.included.push_back(item);
                summary.total_included_tokens += parsed->final_size;
                break;
            case ContextAuditAction::Excluded:
                summary.excluded.push_back(item);
                break;
            case ContextAuditAction::Compressed:
                summary.compressed.push_back(item);
                summary.total_included_tokens += parsed->final_size;
                break;
        }
    }
    return summary;
}

} // namespace aistudio::core
