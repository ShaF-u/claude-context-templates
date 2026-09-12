#include "test_framework.hpp"
#include "Core/Util/TextRange.hpp"

#include <string>
#include <vector>

using namespace aistudio::core;

AISTUDIO_TEST(TextRange_CountLines_EmptyContentHasNoLines) {
    AISTUDIO_EXPECT(CountLines("") == 0);
}

AISTUDIO_TEST(TextRange_CountLines_TrailingNewlineDoesNotAddALine) {
    AISTUDIO_EXPECT(CountLines("a\nb") == 2);
    AISTUDIO_EXPECT(CountLines("a\nb\n") == 2);
}

AISTUDIO_TEST(TextRange_CountLines_CountsBlankLinesInTheMiddle) {
    AISTUDIO_EXPECT(CountLines("a\n\nb\n") == 3);
}

AISTUDIO_TEST(TextRange_ExtractLines_ReturnsInclusiveRange) {
    const std::string content = "one\ntwo\nthree\nfour\n";
    AISTUDIO_EXPECT(ExtractLines(content, 2, 3) == "two\nthree");
}

AISTUDIO_TEST(TextRange_ExtractLines_SingleLine) {
    AISTUDIO_EXPECT(ExtractLines("one\ntwo\n", 1, 1) == "one");
}

AISTUDIO_TEST(TextRange_ExtractLines_ClampsPastEndOfFile) {
    AISTUDIO_EXPECT(ExtractLines("one\ntwo\n", 2, 99) == "two");
}

AISTUDIO_TEST(TextRange_ExtractLines_RangeEntirelyPastEndIsEmpty) {
    AISTUDIO_EXPECT(ExtractLines("one\ntwo\n", 50, 60).empty());
}

AISTUDIO_TEST(TextRange_ExtractLines_InvertedRangeIsEmpty) {
    AISTUDIO_EXPECT(ExtractLines("one\ntwo\n", 2, 1).empty());
}

AISTUDIO_TEST(TextRange_ExtractLines_DropsCarriageReturnFromCrlfContent) {
    // Files are read in binary (FileScanner/FileCache/context_fetch all
    // do), so a CRLF source arrives with its '\r' still attached.
    AISTUDIO_EXPECT(ExtractLines("one\r\ntwo\r\n", 1, 2) == "one\ntwo");
}

AISTUDIO_TEST(TextRange_MergeLineRanges_MergesOverlapping) {
    const auto merged = MergeLineRanges({{1, 10}, {5, 20}});
    AISTUDIO_EXPECT(merged.size() == 1);
    AISTUDIO_EXPECT(merged[0].start == 1);
    AISTUDIO_EXPECT(merged[0].end == 20);
}

AISTUDIO_TEST(TextRange_MergeLineRanges_MergesAdjacent) {
    // [1,5] and [6,9] touch: keeping them separate would spend more on a
    // block header than the gap is worth.
    const auto merged = MergeLineRanges({{1, 5}, {6, 9}});
    AISTUDIO_EXPECT(merged.size() == 1);
    AISTUDIO_EXPECT(merged[0].end == 9);
}

AISTUDIO_TEST(TextRange_MergeLineRanges_KeepsSeparateBlocksApart) {
    const auto merged = MergeLineRanges({{1, 5}, {40, 50}});
    AISTUDIO_EXPECT(merged.size() == 2);
    AISTUDIO_EXPECT(merged[0].end == 5);
    AISTUDIO_EXPECT(merged[1].start == 40);
}

AISTUDIO_TEST(TextRange_MergeLineRanges_SortsUnorderedInput) {
    const auto merged = MergeLineRanges({{40, 50}, {1, 5}});
    AISTUDIO_EXPECT(merged.size() == 2);
    AISTUDIO_EXPECT(merged[0].start == 1);
}

AISTUDIO_TEST(TextRange_MergeLineRanges_DropsInvalidRanges) {
    const auto merged = MergeLineRanges({{0, 5}, {10, 4}, {7, 8}});
    AISTUDIO_EXPECT(merged.size() == 1);
    AISTUDIO_EXPECT(merged[0].start == 7);
    AISTUDIO_EXPECT(merged[0].end == 8);
}

AISTUDIO_TEST(TextRange_BuildExcerpt_LabelsEachBlockWithItsLineNumbers) {
    const std::string content = "l1\nl2\nl3\nl4\nl5\nl6\n";
    const auto excerpt = BuildExcerpt("a.txt", content, {{2, 3}});
    AISTUDIO_EXPECT(excerpt == "@@ a.txt:2-3 @@\nl2\nl3");
}

AISTUDIO_TEST(TextRange_BuildExcerpt_EmitsOneBlockPerSeparateRange) {
    const std::string content = "l1\nl2\nl3\nl4\nl5\nl6\n";
    const auto excerpt = BuildExcerpt("a.txt", content, {{1, 1}, {5, 6}});
    AISTUDIO_EXPECT(excerpt.find("@@ a.txt:1-1 @@") != std::string::npos);
    AISTUDIO_EXPECT(excerpt.find("@@ a.txt:5-6 @@") != std::string::npos);
    // The middle of the file is genuinely absent -- that is the point.
    AISTUDIO_EXPECT(excerpt.find("l3") == std::string::npos);
}

AISTUDIO_TEST(TextRange_BuildExcerpt_ClampsWindowsToTheFileBeforeMerging) {
    // Two windows that both run off the end of a 3-line file are the same
    // block once clamped, and must not come back as two.
    const std::string content = "l1\nl2\nl3\n";
    const auto excerpt = BuildExcerpt("a.txt", content, {{-5, 2}, {2, 99}});
    AISTUDIO_EXPECT(excerpt == "@@ a.txt:1-3 @@\nl1\nl2\nl3");
}

AISTUDIO_TEST(TextRange_BuildExcerpt_EmptyContentIsEmpty) {
    AISTUDIO_EXPECT(BuildExcerpt("a.txt", "", {{1, 5}}).empty());
}
