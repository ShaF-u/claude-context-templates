#include "Core/Util/Identifier.hpp"

#include <cctype>

namespace aistudio::core {

bool IsIdentifierChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string TrailingIdentifier(const std::string& text) {
    std::size_t end = text.size();
    while (end > 0 && !IsIdentifierChar(text[end - 1])) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && IsIdentifierChar(text[start - 1])) {
        --start;
    }
    return text.substr(start, end - start);
}

std::optional<std::pair<std::size_t, std::size_t>> IdentifierRangeAt(const std::string& text, std::size_t byte_offset) {
    const std::size_t offset = byte_offset > text.size() ? text.size() : byte_offset;
    if (offset < text.size() && IsIdentifierChar(text[offset])) {
        std::size_t start = offset;
        std::size_t end = offset;
        while (end < text.size() && IsIdentifierChar(text[end])) {
            ++end;
        }
        while (start > 0 && IsIdentifierChar(text[start - 1])) {
            --start;
        }
        return std::make_pair(start, end);
    }
    // Falls back to the trailing identifier run before `offset` — same
    // scan TrailingIdentifier(text.substr(0, offset)) performs, done
    // directly against `text` itself (rather than a substr copy) so the
    // resulting indices are already in `text`'s own coordinate space.
    std::size_t end = offset;
    while (end > 0 && !IsIdentifierChar(text[end - 1])) {
        --end;
    }
    std::size_t start = end;
    while (start > 0 && IsIdentifierChar(text[start - 1])) {
        --start;
    }
    if (start == end) {
        return std::nullopt;
    }
    return std::make_pair(start, end);
}

std::string IdentifierAt(const std::string& text, std::size_t byte_offset) {
    const auto range = IdentifierRangeAt(text, byte_offset);
    if (!range.has_value()) {
        return {};
    }
    return text.substr(range->first, range->second - range->first);
}

} // namespace aistudio::core
