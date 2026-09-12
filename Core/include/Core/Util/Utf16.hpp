#pragma once

#include <cstddef>
#include <string>

namespace aistudio::core {

// UTF-16 <-> UTF-8 position conversion for LSP's `character` field (LSP
// Position.character counts UTF-16 code units within a line -- the
// protocol's default `positionEncoding`, and the only one
// Core/LSP/LspServer advertises support for). Every other position
// representation already in this project (Symbol::line,
// AstNode::start_line/end_line, ReferenceEdge::line, ...) is line-only
// with no column at all, so this conversion exists purely for the new
// LSP layer's own within-a-line byte<->UTF-16-unit mapping -- it isn't
// used to resolve anything against those existing indexes directly.

// Converts a UTF-16 code-unit offset within `line` (a single line of
// UTF-8-encoded text, no embedded '\n') to the equivalent UTF-8 byte
// offset. Clamps to line.size() if `utf16_offset` runs past the line's
// own length (lenient rather than throwing -- a stale/out-of-range
// Position from a client editing a since-changed document is an
// expected occurrence, not a protocol violation). A `utf16_offset` that
// would land on the low half of a surrogate pair (splitting one
// codepoint in two) is rounded down to that codepoint's own start byte.
[[nodiscard]] std::size_t Utf16OffsetToUtf8Byte(const std::string& line, std::size_t utf16_offset);

// Inverse of the above: converts a UTF-8 byte offset within `line` to
// the equivalent UTF-16 code-unit offset. Clamps to the line's full
// UTF-16 length if `utf8_byte_offset` runs past line.size(). A
// `utf8_byte_offset` that lands mid-codepoint (not on a UTF-8 sequence's
// first byte) behaves as if it had been rounded down to that
// codepoint's own start byte.
[[nodiscard]] std::size_t Utf8ByteToUtf16Offset(const std::string& line, std::size_t utf8_byte_offset);

} // namespace aistudio::core
