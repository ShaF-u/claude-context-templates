#include "test_framework.hpp"
#include "Core/Util/Utf8.hpp"

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
