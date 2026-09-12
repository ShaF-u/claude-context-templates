#include "test_framework.hpp"
#include "Core/Context/ContextItem.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(EstimateTokens_EmptyContent_IsZero) {
    AISTUDIO_EXPECT(EstimateTokens("") == 0);
}

AISTUDIO_TEST(EstimateTokens_NonEmptyContent_IsAtLeastOne) {
    AISTUDIO_EXPECT(EstimateTokens("a") == 1);
}

AISTUDIO_TEST(EstimateTokens_ScalesWithLength) {
    const std::string short_text = "hi";
    const std::string long_text(400, 'x');
    AISTUDIO_EXPECT(EstimateTokens(long_text) > EstimateTokens(short_text));
    AISTUDIO_EXPECT(EstimateTokens(long_text) == 100);
}

AISTUDIO_TEST(ToString_CompressionLevel_Symbol_ReturnsSymbol) {
    AISTUDIO_EXPECT(ToString(CompressionLevel::Symbol) == "Symbol");
}

AISTUDIO_TEST(ContextSourceKindFromString_RoundTripsEveryKnownKind) {
    AISTUDIO_EXPECT(ContextSourceKindFromString(ToString(ContextSourceKind::File)) == ContextSourceKind::File);
    AISTUDIO_EXPECT(ContextSourceKindFromString(ToString(ContextSourceKind::Symbol)) == ContextSourceKind::Symbol);
    AISTUDIO_EXPECT(ContextSourceKindFromString(ToString(ContextSourceKind::Dependency)) ==
                     ContextSourceKind::Dependency);
    AISTUDIO_EXPECT(ContextSourceKindFromString(ToString(ContextSourceKind::GitDiff)) == ContextSourceKind::GitDiff);
    AISTUDIO_EXPECT(ContextSourceKindFromString(ToString(ContextSourceKind::Custom)) == ContextSourceKind::Custom);
}

AISTUDIO_TEST(ContextSourceKindFromString_UnrecognizedText_FallsBackToCustom) {
    // An unknown kind (a future value this build doesn't know about yet,
    // or a corrupted row) means "no dedicated restoration source" --
    // exactly what Custom already means, not a guess like File.
    AISTUDIO_EXPECT(ContextSourceKindFromString("SomethingFromTheFuture") == ContextSourceKind::Custom);
    AISTUDIO_EXPECT(ContextSourceKindFromString("") == ContextSourceKind::Custom);
}
