#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace aistudio::core {

// Tracks which ContextItem content has already been sent to the
// connected AI within this MCP session, so an unchanged item can be
// replaced with a small stub instead of resending its full content
// (docs/ROADMAP.md CE-4 "セッション既送信台帳"). One instance per
// McpServer process (stdio == one session), passed via
// McpServerOptions::sent_ledger, opt-in via mcp.suppress_resent_content.
class SentLedger {
public:
    struct CheckResult {
        bool unchanged = false;
        std::int64_t sent_at_ordinal = 0; // valid only when unchanged
    };

    // Call once per context_retrieve invocation, before any Check() calls
    // for that response.
    std::int64_t BeginResponse();

    // Compares `content` against what was last recorded as sent for `id`.
    // Does not mutate state -- call RecordSent() separately for items that
    // are actually included in the response.
    [[nodiscard]] CheckResult Check(const std::string& id, const std::string& content) const;

    // Records `id`+`content` as sent as of the current response ordinal
    // (the value BeginResponse() most recently returned).
    void RecordSent(const std::string& id, const std::string& content);

private:
    struct Entry {
        std::size_t content_hash = 0;
        std::int64_t ordinal = 0;
    };

    std::unordered_map<std::string, Entry> entries_;
    std::int64_t current_ordinal_ = 0;
};

} // namespace aistudio::core
