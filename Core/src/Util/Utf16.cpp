#include "Core/Util/Utf16.hpp"

#include <cstdint>

namespace aistudio::core {

namespace {

struct DecodedCodepoint {
    std::uint32_t codepoint;
    std::size_t byte_length; // 1-4
};

// Decodes the UTF-8 codepoint starting at `text[byte_offset]`.
// Malformed or truncated sequences decode as a single replacement byte
// (length 1, codepoint U+FFFD) rather than throwing -- this function
// only ever sees real source lines already read as `std::string` (which
// this project's SymbolIndex/AstIndex assume are well-formed UTF-8 to
// begin with), so this is a defensive fallback, not a path exercised by
// legitimate input.
DecodedCodepoint DecodeUtf8(const std::string& text, std::size_t byte_offset) {
    const auto remaining = text.size() - byte_offset;
    const auto first = static_cast<unsigned char>(text[byte_offset]);
    std::size_t length = 1;
    std::uint32_t codepoint = first;
    if ((first & 0x80) == 0x00) {
        length = 1;
        codepoint = first;
    } else if ((first & 0xE0) == 0xC0 && remaining >= 2) {
        length = 2;
        codepoint = first & 0x1Fu;
    } else if ((first & 0xF0) == 0xE0 && remaining >= 3) {
        length = 3;
        codepoint = first & 0x0Fu;
    } else if ((first & 0xF8) == 0xF0 && remaining >= 4) {
        length = 4;
        codepoint = first & 0x07u;
    } else {
        return {0xFFFD, 1};
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto continuation = static_cast<unsigned char>(text[byte_offset + i]);
        if ((continuation & 0xC0) != 0x80) {
            return {0xFFFD, 1};
        }
        codepoint = (codepoint << 6) | (continuation & 0x3Fu);
    }
    return {codepoint, length};
}

std::size_t Utf16UnitsFor(std::uint32_t codepoint) { return codepoint > 0xFFFFu ? 2 : 1; }

} // namespace

std::size_t Utf16OffsetToUtf8Byte(const std::string& line, std::size_t utf16_offset) {
    std::size_t byte_index = 0;
    std::size_t utf16_index = 0;
    while (byte_index < line.size() && utf16_index < utf16_offset) {
        const auto decoded = DecodeUtf8(line, byte_index);
        const auto units = Utf16UnitsFor(decoded.codepoint);
        if (utf16_index + units > utf16_offset) {
            break; // target offset falls inside a surrogate pair -- round down
        }
        byte_index += decoded.byte_length;
        utf16_index += units;
    }
    return byte_index;
}

std::size_t Utf8ByteToUtf16Offset(const std::string& line, std::size_t utf8_byte_offset) {
    std::size_t byte_index = 0;
    std::size_t utf16_index = 0;
    while (byte_index < line.size() && byte_index < utf8_byte_offset) {
        const auto decoded = DecodeUtf8(line, byte_index);
        byte_index += decoded.byte_length;
        utf16_index += Utf16UnitsFor(decoded.codepoint);
    }
    return utf16_index;
}

} // namespace aistudio::core
