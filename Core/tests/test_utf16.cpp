#include "test_framework.hpp"
#include "Core/Util/Utf16.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(Utf16OffsetToUtf8Byte_AsciiOnly_IsIdentity) {
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte("hello world", 3) == 3);
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte("hello world", 0) == 0);
}

AISTUDIO_TEST(Utf8ByteToUtf16Offset_AsciiOnly_IsIdentity) {
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset("hello world", 3) == 3);
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset("hello world", 0) == 0);
}

AISTUDIO_TEST(Utf16OffsetToUtf8Byte_ClampsPastEndOfLine) {
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte("abc", 100) == 3);
}

AISTUDIO_TEST(Utf8ByteToUtf16Offset_ClampsPastEndOfLine) {
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset("abc", 100) == 3);
}

AISTUDIO_TEST(Utf16OffsetToUtf8Byte_BmpMultiByteCharacters_EachCountsAsOneUtf16Unit) {
    // "日本語abc" -- each of 日/本/語 is 3 UTF-8 bytes but exactly 1 UTF-16
    // code unit (all three are in the Basic Multilingual Plane).
    const std::string line = "\xE6\x97\xA5" "\xE6\x9C\xAC" "\xE8\xAA\x9E" "abc";
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 0) == 0);
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 1) == 3);  // after 日, before 本
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 3) == 9);  // after 語, before 'a'
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 4) == 10); // after 'a'
}

AISTUDIO_TEST(Utf8ByteToUtf16Offset_BmpMultiByteCharacters_EachCountsAsOneUtf16Unit) {
    const std::string line = "\xE6\x97\xA5" "\xE6\x9C\xAC" "\xE8\xAA\x9E" "abc";
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 0) == 0);
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 3) == 1);
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 9) == 3);
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 10) == 4);
}

AISTUDIO_TEST(Utf16OffsetToUtf8Byte_SupplementaryPlaneCharacter_CountsAsTwoUtf16Units) {
    // U+1F600 (an emoji outside the BMP) is 4 UTF-8 bytes but 2 UTF-16
    // code units (a surrogate pair) -- the case a naive byte-count or
    // codepoint-count conversion gets wrong.
    const std::string line = "a" "\xF0\x9F\x98\x80" "b";
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 0) == 0);
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 1) == 1); // after 'a', before the emoji
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 3) == 5); // after the emoji's 2 units, before 'b'
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte(line, 4) == 6); // after 'b'
}

AISTUDIO_TEST(Utf8ByteToUtf16Offset_SupplementaryPlaneCharacter_CountsAsTwoUtf16Units) {
    const std::string line = "a" "\xF0\x9F\x98\x80" "b";
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 0) == 0);
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 1) == 1);
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 5) == 3);
    AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, 6) == 4);
}

AISTUDIO_TEST(Utf16OffsetToUtf8Byte_RoundTripsWithUtf8ByteToUtf16Offset) {
    const std::string line = "\xE6\x97\xA5" "\xE6\x9C\xAC" "\xE8\xAA\x9E" "abc";
    for (std::size_t utf16 = 0; utf16 <= 4; ++utf16) {
        const auto byte_offset = Utf16OffsetToUtf8Byte(line, utf16);
        AISTUDIO_EXPECT(Utf8ByteToUtf16Offset(line, byte_offset) == utf16);
    }
}

AISTUDIO_TEST(Utf16OffsetToUtf8Byte_EmptyLine_ReturnsZero) {
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte("", 0) == 0);
    AISTUDIO_EXPECT(Utf16OffsetToUtf8Byte("", 5) == 0);
}
