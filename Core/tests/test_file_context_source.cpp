#include "test_framework.hpp"
#include "Core/Context/FileContextSource.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(MakeFileContextItem_FromMetadataOnly_EstimatesFromSize) {
    FileMetadata metadata;
    metadata.path = "src/main.cpp";
    metadata.size = 400;

    const auto item = MakeFileContextItem(metadata, /*priority=*/75);

    AISTUDIO_EXPECT(item.id == "src/main.cpp");
    AISTUDIO_EXPECT(item.source == ContextSourceKind::File);
    AISTUDIO_EXPECT(item.priority == 75);
    AISTUDIO_EXPECT(item.estimated_tokens == 100);
    AISTUDIO_EXPECT(item.content.empty());
}

AISTUDIO_TEST(MakeFileContextItem_WithContent_EstimatesFromContent) {
    FileMetadata metadata;
    metadata.path = "src/main.cpp";
    metadata.size = 999; // deliberately wrong/stale to prove content wins

    const auto item = MakeFileContextItem(metadata, std::string("int main() {}"), /*priority=*/60);

    AISTUDIO_EXPECT(item.content == "int main() {}");
    AISTUDIO_EXPECT(item.estimated_tokens == EstimateTokens("int main() {}"));
}

AISTUDIO_TEST(MakeFileContextItem_DefaultPriority_IsFifty) {
    FileMetadata metadata;
    metadata.path = "a.txt";
    const auto item = MakeFileContextItem(metadata);
    AISTUDIO_EXPECT(item.priority == 50);
}
