#include "Core/Context/ContextCompressor.hpp"

#include <algorithm>

namespace aistudio::core {

namespace {

bool IsUtf8Continuation(char c) {
    return (static_cast<unsigned char>(c) & 0xC0) == 0x80;
}

// Largest offset <= `offset` that starts a UTF-8 character — cutting on a
// continuation byte would split a multi-byte character.
std::size_t Utf8FloorBoundary(const std::string& text, std::size_t offset) {
    while (offset > 0 && offset < text.size() && IsUtf8Continuation(text[offset])) {
        --offset;
    }
    return offset;
}

std::size_t Utf8CeilBoundary(const std::string& text, std::size_t offset) {
    while (offset < text.size() && IsUtf8Continuation(text[offset])) {
        ++offset;
    }
    return offset;
}

} // namespace

ContextItem ContextCompressor::Compress(ContextItem item, std::int64_t target_tokens) const {
    if (target_tokens <= 0 || item.estimated_tokens <= target_tokens) {
        return item;
    }

    static const std::string kMarkerPrefix = "\n... [omitted ";
    static const std::string kMarkerSuffix = " chars] ...\n";

    // ~4 chars/token, the same approximation EstimateTokens() uses.
    const std::int64_t target_chars = target_tokens * 4;
    const std::int64_t marker_overhead =
        static_cast<std::int64_t>(kMarkerPrefix.size() + kMarkerSuffix.size()) + 10; // digits of the omitted count
    const std::int64_t available = target_chars - marker_overhead;

    if (available <= 0 || static_cast<std::int64_t>(item.content.size()) <= available) {
        // Too small a budget to cut anything usefully, or content
        // already fits in character terms — leave it as-is. If it still
        // doesn't fit the caller's budget, ContextSelector excludes it.
        return item;
    }

    const auto requested_head = static_cast<std::size_t>(available * 6 / 10);
    const auto requested_tail = static_cast<std::size_t>(available) - requested_head;

    // Both cuts only ever move in the shrinking direction, so the result
    // still fits target_tokens.
    const auto head_chars = Utf8FloorBoundary(item.content, requested_head);
    const auto tail_start = Utf8CeilBoundary(item.content, item.content.size() - requested_tail);
    const auto tail_chars = item.content.size() - tail_start;

    const std::string head = item.content.substr(0, head_chars);
    const std::string tail = item.content.substr(tail_start);
    const auto omitted = item.content.size() - head_chars - tail_chars;

    item.content = head + kMarkerPrefix + std::to_string(omitted) + kMarkerSuffix + tail;
    item.compression = CompressionLevel::Summary;
    item.estimated_tokens = EstimateTokens(item.content);
    return item;
}

} // namespace aistudio::core
