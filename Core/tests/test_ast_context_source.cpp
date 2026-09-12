#include "test_framework.hpp"
#include "Core/Context/AstContextSource.hpp"
#include "Core/Index/AstExtractor.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(MakeAstContextItem_SetsSourceAndCompressionLevel) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");

    const auto item = MakeAstContextItem("a.hpp", root);

    AISTUDIO_EXPECT(item.source == ContextSourceKind::File);
    AISTUDIO_EXPECT(item.compression == CompressionLevel::Ast);
    AISTUDIO_EXPECT(item.id == "a.hpp");
    AISTUDIO_EXPECT(item.estimated_tokens > 0);
}

AISTUDIO_TEST(MakeAstContextItem_ContentIncludesNodeKindsAndText) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");

    const auto item = MakeAstContextItem("a.hpp", root);

    AISTUDIO_EXPECT(item.content.find("translation_unit") != std::string::npos);
    AISTUDIO_EXPECT(item.content.find("class_specifier") != std::string::npos);
    AISTUDIO_EXPECT(item.content.find("Foo") != std::string::npos);
}

AISTUDIO_TEST(MakeAstContextItem_DefaultPriority_IsFiftyFive) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    const auto item = MakeAstContextItem("a.hpp", root);
    AISTUDIO_EXPECT(item.priority == 55);
}

AISTUDIO_TEST(MakeAstContextItem_CustomPriority_IsRespected) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    const auto item = MakeAstContextItem("a.hpp", root, 80);
    AISTUDIO_EXPECT(item.priority == 80);
}

AISTUDIO_TEST(MakeAstContextItem_DependsOnItsOwnFile) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    const auto item = MakeAstContextItem("a.hpp", root);
    AISTUDIO_EXPECT(item.depends_on.size() == 1);
    AISTUDIO_EXPECT(item.depends_on.front() == "a.hpp");
}

AISTUDIO_TEST(MakeAstContextItem_DeeplyNestedTree_IsTruncated) {
    // A left-leaning chain of nested parenthesized expressions goes many
    // levels deeper than kMaxDepth, so the renderer's depth cap should
    // cut it off with a marker rather than expanding the whole thing.
    const AstExtractor extractor;
    const std::string content = "int x = (((((((((1)))))))));\n";
    const auto root = extractor.Extract(content);

    const auto item = MakeAstContextItem("a.cpp", root);
    AISTUDIO_EXPECT(item.content.find("truncated at depth") != std::string::npos);
}

AISTUDIO_TEST(MakeAstContextItem_EmptyFile_ProducesRootLineOnly) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("");
    const auto item = MakeAstContextItem("empty.cpp", root);
    AISTUDIO_EXPECT(item.content.find('\n') == std::string::npos);
    AISTUDIO_EXPECT(item.content.find("translation_unit") != std::string::npos);
}
