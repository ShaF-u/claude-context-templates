#include "test_framework.hpp"
#include "Core/Util/Utf8.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace aistudio::core;

AISTUDIO_TEST(IsValidUtf8_EmptyString_IsValid) {
    AISTUDIO_EXPECT(IsValidUtf8(""));
}

AISTUDIO_TEST(IsValidUtf8_PlainAscii_IsValid) {
    AISTUDIO_EXPECT(IsValidUtf8("class PlayerAttack {\n};\n"));
}

AISTUDIO_TEST(IsValidUtf8_JapaneseText_IsValid) {
    AISTUDIO_EXPECT(IsValidUtf8("このファイルは日本語のコメントです"));
}

AISTUDIO_TEST(IsValidUtf8_LoneContinuationByte_IsInvalid) {
    // 0x80 alone is a continuation byte with no lead byte -- structurally
    // invalid UTF-8.
    const std::string text = "abc\x80xyz";
    AISTUDIO_EXPECT(!IsValidUtf8(text));
}

AISTUDIO_TEST(IsValidUtf8_TruncatedMultibyteSequence_IsInvalid) {
    // 0xE3 starts a 3-byte sequence but is immediately cut off.
    const std::string text = "abc\xE3xyz";
    AISTUDIO_EXPECT(!IsValidUtf8(text));
}

AISTUDIO_TEST(IsValidUtf8_ZipCentralDirectoryLikeBytes_IsInvalid) {
    // A byte sequence in the same family as what a real .jar/.zip's
    // compressed data produced in this project's own bug report
    // (docs/ROADMAP.md CE-5) -- readable ASCII filenames interleaved with
    // arbitrary binary bytes from compressed entries.
    std::string text = "org/gradle/wrapper/GradleWrapperMain.class";
    text += '\x00';
    text += '\xFA';
    text += '\x17';
    text += "more";
    AISTUDIO_EXPECT(!IsValidUtf8(text));
}

AISTUDIO_TEST(Utf8SafeTruncationLength_MaxBytesAtOrPastEnd_ReturnsFullLength) {
    const std::string text = "hello";
    AISTUDIO_EXPECT(Utf8SafeTruncationLength(text, text.size()) == text.size());
    AISTUDIO_EXPECT(Utf8SafeTruncationLength(text, text.size() + 10) == text.size());
}

AISTUDIO_TEST(Utf8SafeTruncationLength_CutOnAsciiBoundary_IsUnchanged) {
    const std::string text = "hello world";
    AISTUDIO_EXPECT(Utf8SafeTruncationLength(text, 5) == 5);
}

AISTUDIO_TEST(Utf8SafeTruncationLength_CutLandsMidMultibyteCharacter_BacksUpToCharacterStart) {
    // "ab" (2 bytes) + U+3042 "あ" (3 bytes, at offsets 2-4). A cut at
    // offset 3 lands on the character's middle continuation byte.
    const std::string text = "ab\xE3\x81\x82" "cd";
    AISTUDIO_EXPECT(Utf8SafeTruncationLength(text, 3) == 2);
    AISTUDIO_EXPECT(Utf8SafeTruncationLength(text, 4) == 2);
}

AISTUDIO_TEST(Utf8SafeTruncationLength_CutLandsExactlyOnCharacterBoundary_IsUnchanged) {
    const std::string text = "ab\xE3\x81\x82" "cd"; // "あ" occupies bytes [2,5)
    AISTUDIO_EXPECT(Utf8SafeTruncationLength(text, 5) == 5);
}

AISTUDIO_TEST(Utf8ToWide_EmptyString_ReturnsEmpty) {
    AISTUDIO_EXPECT(Utf8ToWide("").empty());
}

AISTUDIO_TEST(Utf8ToWide_PlainAscii_MatchesWideLiteral) {
    AISTUDIO_EXPECT(Utf8ToWide("hello world") == L"hello world");
}

AISTUDIO_TEST(Utf8ToWide_JapaneseText_MatchesWideLiteral) {
    AISTUDIO_EXPECT(Utf8ToWide("こんにちは") == L"こんにちは");
}

AISTUDIO_TEST(WideToUtf8_EmptyString_ReturnsEmpty) {
    AISTUDIO_EXPECT(WideToUtf8(L"").empty());
}

AISTUDIO_TEST(WideToUtf8_PlainAscii_MatchesUtf8Literal) {
    AISTUDIO_EXPECT(WideToUtf8(L"hello world") == "hello world");
}

AISTUDIO_TEST(WideToUtf8_JapaneseText_MatchesUtf8Literal) {
    AISTUDIO_EXPECT(WideToUtf8(L"こんにちは") == "こんにちは");
}

AISTUDIO_TEST(WideToUtf8_RoundTripsThroughUtf8ToWide) {
    const std::string original = "mixed ASCII と日本語 123";
    AISTUDIO_EXPECT(WideToUtf8(Utf8ToWide(original)) == original);
}

AISTUDIO_TEST(Cp932ToUtf8_EmptyString_ReturnsEmpty) {
    const auto result = Cp932ToUtf8("");
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(result->empty());
}

AISTUDIO_TEST(Cp932ToUtf8_PlainAscii_IsUnchanged) {
    const auto result = Cp932ToUtf8("class PlayerAttack {\n};\n");
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(*result == "class PlayerAttack {\n};\n");
}

AISTUDIO_TEST(Cp932ToUtf8_KnownShiftJisBytes_DecodesToExpectedUtf8) {
    // 0x82 0xA0 is "あ" (U+3042) in CP932 -- a single-character fixture
    // simple enough to verify by hand against any Shift-JIS code table.
    const std::string cp932_bytes = "\x82\xA0";
    const auto result = Cp932ToUtf8(cp932_bytes);
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(*result == "\xE3\x81\x82"); // "あ" in UTF-8
}

AISTUDIO_TEST(Cp932ToUtf8_RoundTripsThroughSourceEncodingLikeContent) {
    // A source line shaped like what this project's own CLAUDE.md warns
    // most Engine/Source .cpp/.hpp files use: CP932 bytes for a Japanese
    // comment mixed with plain ASCII code.
    const std::string utf8_original = "// シングルトンクラス\nclass Foo {};\n";
    // Encode utf8_original to CP932 bytes via the Win32 API directly
    // (there's no CP932-encode helper in this codebase to reuse -- only
    // the decode direction is needed anywhere else), then verify
    // Cp932ToUtf8 recovers the original UTF-8 text exactly.
#if defined(_WIN32)
    const std::wstring wide = Utf8ToWide(utf8_original);
    const int cp932_length =
        ::WideCharToMultiByte(932, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string cp932(static_cast<std::size_t>(cp932_length), '\0');
    ::WideCharToMultiByte(932, 0, wide.data(), static_cast<int>(wide.size()), cp932.data(), cp932_length, nullptr,
                          nullptr);
    const auto result = Cp932ToUtf8(cp932);
    AISTUDIO_EXPECT(result.has_value());
    AISTUDIO_EXPECT(*result == utf8_original);
#endif
}

AISTUDIO_TEST(Cp932ToUtf8_ByteSequenceInvalidInBothEncodings_ReturnsNullopt) {
    // 0x81 is a valid CP932 lead byte, but 0xFF is outside the valid
    // trail-byte range for it -- not decodable as CP932 either, so this
    // should still be rejected as genuinely binary content, not silently
    // turned into replacement characters.
    const std::string text = "abc\x81\xFFxyz";
    AISTUDIO_EXPECT(!Cp932ToUtf8(text).has_value());
}

AISTUDIO_TEST(Utf8SafeTruncationLength_ResultIsAlwaysValidUtf8) {
    std::string text = "prefix ";
    for (int i = 0; i < 20; ++i) {
        text += "\xE3\x81\x82"; // "あ"
    }
    for (std::size_t cut = 0; cut <= text.size(); ++cut) {
        const auto length = Utf8SafeTruncationLength(text, cut);
        AISTUDIO_EXPECT(IsValidUtf8(text.substr(0, length)));
    }
}
