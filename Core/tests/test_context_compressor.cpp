#include "test_framework.hpp"
#include "Core/Context/ContextCompressor.hpp"

using namespace aistudio::core;

namespace {
ContextItem MakeItem(std::string content) {
    ContextItem item;
    item.id = "item";
    item.content = std::move(content);
    item.estimated_tokens = EstimateTokens(item.content);
    return item;
}
} // namespace

AISTUDIO_TEST(ContextCompressor_Compress_AlreadyFits_ReturnsUnchanged) {
    ContextCompressor compressor;
    const auto item = MakeItem("short");

    const auto result = compressor.Compress(item, /*target_tokens=*/100);

    AISTUDIO_EXPECT(result.content == "short");
    AISTUDIO_EXPECT(result.compression == CompressionLevel::Raw);
}

AISTUDIO_TEST(ContextCompressor_Compress_NonPositiveTarget_ReturnsUnchanged) {
    ContextCompressor compressor;
    const auto item = MakeItem("some content");

    const auto result = compressor.Compress(item, /*target_tokens=*/0);

    AISTUDIO_EXPECT(result.content == "some content");
}

AISTUDIO_TEST(ContextCompressor_Compress_OversizedContent_ProducesSmallerSummary) {
    ContextCompressor compressor;
    const std::string head = "BEGIN_MARKER_";
    const std::string middle(2000, 'x');
    const std::string tail = "_END_MARKER";
    const auto item = MakeItem(head + middle + tail);

    const auto result = compressor.Compress(item, /*target_tokens=*/50);

    AISTUDIO_EXPECT(result.compression == CompressionLevel::Summary);
    AISTUDIO_EXPECT(result.estimated_tokens <= 50);
    AISTUDIO_EXPECT(result.content.size() < item.content.size());
    // Keeps a prefix of the head and a suffix of the tail.
    AISTUDIO_EXPECT(result.content.find("BEGIN_MARKER_") == 0);
    AISTUDIO_EXPECT(result.content.find("_END_MARKER") != std::string::npos);
    AISTUDIO_EXPECT(result.content.find("omitted") != std::string::npos);
}

AISTUDIO_TEST(ContextCompressor_Compress_TargetTooSmallForMarkerOverhead_ReturnsUnchanged) {
    ContextCompressor compressor;
    // estimated_tokens (5) exceeds target_tokens (2), so compression is
    // attempted, but the marker text alone wouldn't fit in the resulting
    // character budget — nothing sensible to cut, so it's left as-is.
    const auto item = MakeItem("twenty characters!!!");

    const auto result = compressor.Compress(item, /*target_tokens=*/2);

    AISTUDIO_EXPECT(result.content == "twenty characters!!!");
}

namespace {

bool IsValidUtf8(const std::string& text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto byte = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        if (byte < 0x80) {
            extra = 0;
        } else if ((byte & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false; // lone continuation byte or invalid leading byte
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

} // namespace

AISTUDIO_TEST(ContextCompressor_Compress_MultibyteUtf8_DoesNotSplitACharacter) {
    // Regression: the cut used to land on a raw byte offset, splitting
    // 3-byte Japanese characters into mojibake fragments.
    ContextCompressor compressor;
    std::string content;
    for (int i = 0; i < 400; ++i) {
        content += "あいうえお"; // 15 bytes per iteration, 3 per character
    }
    const auto item = MakeItem(content);

    // Sweep targets so the cut lands at a different offset each time.
    for (std::int64_t target = 40; target <= 60; ++target) {
        const auto result = compressor.Compress(item, target);
        AISTUDIO_EXPECT(result.estimated_tokens <= target);
        AISTUDIO_EXPECT(IsValidUtf8(result.content));
    }
}

AISTUDIO_TEST(ContextCompressor_Compress_MultibyteUtf8_StillFitsTheTargetAfterBoundaryAdjustment) {
    // The adjustment only shrinks, so what fit before still fits.
    ContextCompressor compressor;
    std::string content;
    for (int i = 0; i < 200; ++i) {
        content += "日本語のコメント行です。\n";
    }
    const auto result = compressor.Compress(MakeItem(content), /*target_tokens=*/80);

    AISTUDIO_EXPECT(result.compression == CompressionLevel::Summary);
    AISTUDIO_EXPECT(result.estimated_tokens <= 80);
    AISTUDIO_EXPECT(IsValidUtf8(result.content));
    AISTUDIO_EXPECT(result.content.find("omitted") != std::string::npos);
}
