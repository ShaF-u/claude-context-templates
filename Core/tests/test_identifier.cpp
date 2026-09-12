#include "test_framework.hpp"
#include "Core/Util/Identifier.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(TrailingIdentifier_PlainIdentifier_ReturnsItself) {
    AISTUDIO_EXPECT(TrailingIdentifier("ToString") == "ToString");
}

AISTUDIO_TEST(TrailingIdentifier_MemberAccess_ReturnsFieldName) {
    AISTUDIO_EXPECT(TrailingIdentifier("selector.Select") == "Select");
}

AISTUDIO_TEST(TrailingIdentifier_QualifiedName_ReturnsLastSegment) {
    AISTUDIO_EXPECT(TrailingIdentifier("Foo::Bar") == "Bar");
}

AISTUDIO_TEST(TrailingIdentifier_ArrowAccess_ReturnsFieldName) {
    AISTUDIO_EXPECT(TrailingIdentifier("this->DoThing") == "DoThing");
}

AISTUDIO_TEST(TrailingIdentifier_TrailingPunctuationIsSkipped_IdentifierBeforeItIsFound) {
    // Trailing non-identifier characters (here, the closing paren) are
    // stripped before scanning for the identifier run, so this finds
    // "fnptr" rather than treating the whole expression as unmatched.
    AISTUDIO_EXPECT(TrailingIdentifier("(*fnptr)") == "fnptr");
}

AISTUDIO_TEST(TrailingIdentifier_NoIdentifierCharactersAtAll_ReturnsEmpty) {
    AISTUDIO_EXPECT(TrailingIdentifier("()").empty());
}

AISTUDIO_TEST(TrailingIdentifier_EmptyInput_ReturnsEmpty) {
    AISTUDIO_EXPECT(TrailingIdentifier("").empty());
}

AISTUDIO_TEST(IdentifierAt_OffsetInsideIdentifier_ReturnsWholeRun) {
    AISTUDIO_EXPECT(IdentifierAt("SymbolIndex", 3) == "SymbolIndex");
}

AISTUDIO_TEST(IdentifierAt_OffsetAtStartOfIdentifier_ReturnsWholeRun) {
    AISTUDIO_EXPECT(IdentifierAt("foo bar", 4) == "bar");
}

AISTUDIO_TEST(IdentifierAt_OffsetRightAfterIdentifier_FallsBackToTrailingIdentifier) {
    // Cursor sitting on the space right after "foo" -- the common case
    // when an editor's caret rests immediately after a just-typed word.
    AISTUDIO_EXPECT(IdentifierAt("foo bar", 3) == "foo");
}

AISTUDIO_TEST(IdentifierAt_OffsetAtEndOfText_FallsBackToTrailingIdentifier) {
    AISTUDIO_EXPECT(IdentifierAt("foo", 3) == "foo");
}

AISTUDIO_TEST(IdentifierAt_QualifiedName_ReturnsOnlyTheUnqualifiedSegment) {
    // "Foo::Bar", offset 6 lands on the 'a' inside "Bar" -- the "::"
    // qualifier itself is never part of an identifier run.
    AISTUDIO_EXPECT(IdentifierAt("Foo::Bar", 6) == "Bar");
}

AISTUDIO_TEST(IdentifierAt_OffsetOnWhitespaceWithNothingBefore_ReturnsEmpty) {
    AISTUDIO_EXPECT(IdentifierAt("   ", 1).empty());
}

AISTUDIO_TEST(IdentifierAt_EmptyText_ReturnsEmpty) {
    AISTUDIO_EXPECT(IdentifierAt("", 0).empty());
}

AISTUDIO_TEST(IdentifierAt_OffsetPastEndOfText_ClampsAndFallsBackToTrailingIdentifier) {
    AISTUDIO_EXPECT(IdentifierAt("foo", 100) == "foo");
}

AISTUDIO_TEST(IdentifierRangeAt_OffsetInsideIdentifier_ReturnsWholeRunRange) {
    const auto range = IdentifierRangeAt("SymbolIndex", 3);
    AISTUDIO_EXPECT(range.has_value());
    AISTUDIO_EXPECT(range->first == 0);
    AISTUDIO_EXPECT(range->second == 11);
}

AISTUDIO_TEST(IdentifierRangeAt_OffsetRightAfterIdentifier_FallsBackToTrailingRange) {
    // Same "foo bar" cursor-after-"foo" case IdentifierAt's own test
    // covers, but checking the byte range rather than the text.
    const auto range = IdentifierRangeAt("foo bar", 3);
    AISTUDIO_EXPECT(range.has_value());
    AISTUDIO_EXPECT(range->first == 0);
    AISTUDIO_EXPECT(range->second == 3);
}

AISTUDIO_TEST(IdentifierRangeAt_SecondOccurrenceOnLine_ReturnsThatOccurrencesOwnRange) {
    // "Total = Total + 1" -- offset 10 lands inside the SECOND "Total",
    // range must point at that occurrence, not the first one at offset 0.
    const auto range = IdentifierRangeAt("Total = Total + 1", 10);
    AISTUDIO_EXPECT(range.has_value());
    AISTUDIO_EXPECT(range->first == 8);
    AISTUDIO_EXPECT(range->second == 13);
}

AISTUDIO_TEST(IdentifierRangeAt_OffsetOnWhitespaceWithNothingBefore_ReturnsNullopt) {
    AISTUDIO_EXPECT(!IdentifierRangeAt("   ", 1).has_value());
}

AISTUDIO_TEST(IdentifierRangeAt_EmptyText_ReturnsNullopt) {
    AISTUDIO_EXPECT(!IdentifierRangeAt("", 0).has_value());
}

AISTUDIO_TEST(IdentifierRangeAt_OffsetPastEndOfText_ClampsAndFallsBackToTrailingRange) {
    const auto range = IdentifierRangeAt("foo", 100);
    AISTUDIO_EXPECT(range.has_value());
    AISTUDIO_EXPECT(range->first == 0);
    AISTUDIO_EXPECT(range->second == 3);
}
