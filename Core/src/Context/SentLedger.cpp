#include "Core/Context/SentLedger.hpp"

namespace aistudio::core {

std::int64_t SentLedger::BeginResponse() {
    return ++current_ordinal_;
}

SentLedger::CheckResult SentLedger::Check(const std::string& id, const std::string& content) const {
    const auto it = entries_.find(id);
    if (it == entries_.end()) {
        return {};
    }
    const std::size_t hash = std::hash<std::string>{}(content);
    if (it->second.content_hash != hash) {
        return {};
    }
    return {.unchanged = true, .sent_at_ordinal = it->second.ordinal};
}

void SentLedger::RecordSent(const std::string& id, const std::string& content) {
    entries_[id] = Entry{.content_hash = std::hash<std::string>{}(content), .ordinal = current_ordinal_};
}

} // namespace aistudio::core
