#include "test_framework.hpp"
#include "Core/Index/AstExtractor.hpp"
#include "Core/Util/Utf8.hpp"

#include <functional>

using namespace aistudio::core;

namespace {

bool AnyNode(const AstNode& node, const std::function<bool(const AstNode&)>& predicate) {
    if (predicate(node)) {
        return true;
    }
    for (const auto& child : node.children) {
        if (AnyNode(child, predicate)) {
            return true;
        }
    }
    return false;
}

const AstNode* FindNodeByKind(const AstNode& node, const std::string& kind) {
    if (node.kind == kind) {
        return &node;
    }
    for (const auto& child : node.children) {
        if (const auto* found = FindNodeByKind(child, kind)) {
            return found;
        }
    }
    return nullptr;
}

std::size_t CountNodes(const AstNode& node) {
    std::size_t count = 1;
    for (const auto& child : node.children) {
        count += CountNodes(child);
    }
    return count;
}

} // namespace

AISTUDIO_TEST(AstExtractor_Extract_RootIsTranslationUnit) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    AISTUDIO_EXPECT(root.kind == "translation_unit");
}

AISTUDIO_TEST(AstExtractor_Extract_FindsClassSpecifier) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    AISTUDIO_EXPECT(AnyNode(root, [](const AstNode& n) { return n.kind == "class_specifier"; }));
}

AISTUDIO_TEST(AstExtractor_Extract_LeafCapturesIdentifierText) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    AISTUDIO_EXPECT(AnyNode(root, [](const AstNode& n) { return n.kind == "type_identifier" && n.text == "Foo"; }));
}

// docs/ROADMAP.md CE-5: LeafText()'s length cap used to be a raw
// resize(120), which can split a multi-byte character (this project's own
// source is full of Japanese comments) and produce invalid UTF-8 that
// crashes JSON serialization downstream instead of just truncating.
AISTUDIO_TEST(AstExtractor_Extract_LongCommentWithMultibyteUtf8_TruncatedTextIsStillValidUtf8) {
    // "//" (2 bytes) + 50 * U+3042 "あ" (3 bytes each) -- byte 120 falls
    // mid-character (118 bytes into the multibyte run, not a multiple of 3).
    std::string comment = "//";
    for (int i = 0; i < 50; ++i) {
        comment += "\xE3\x81\x82";
    }
    const std::string source = comment + "\nclass Foo {\n};\n";

    const AstExtractor extractor;
    const auto root = extractor.Extract(source);

    const auto* comment_node = FindNodeByKind(root, "comment");
    AISTUDIO_EXPECT(comment_node != nullptr);
    AISTUDIO_EXPECT(comment_node->text.size() < comment.size()); // actually got truncated
    AISTUDIO_EXPECT(IsValidUtf8(comment_node->text));
}

AISTUDIO_TEST(AstExtractor_Extract_NonLeafNodes_HaveEmptyText) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    AISTUDIO_EXPECT(root.text.empty());
}

AISTUDIO_TEST(AstExtractor_Extract_RecordsOneBasedLineRange) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("// comment\nclass Foo {\n};\n");
    bool found = false;
    for (const auto& child : root.children) {
        if (child.kind == "class_specifier") {
            found = true;
            AISTUDIO_EXPECT(child.start_line == 2);
        }
    }
    AISTUDIO_EXPECT(found);
}

AISTUDIO_TEST(AstExtractor_Extract_MultilineNode_EndLineAfterStartLine) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n    int x;\n};\n");
    bool found = false;
    for (const auto& child : root.children) {
        if (child.kind == "class_specifier") {
            found = true;
            AISTUDIO_EXPECT(child.end_line > child.start_line);
        }
    }
    AISTUDIO_EXPECT(found);
}

AISTUDIO_TEST(AstExtractor_Extract_SkipsAnonymousPunctuationNodes) {
    // Only named nodes (tree-sitter's AST-vs-CST distinction) should
    // appear; punctuation like "{"/"}"/";" is anonymous and excluded.
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n};\n");
    AISTUDIO_EXPECT(!AnyNode(root, [](const AstNode& n) { return n.kind == "{" || n.kind == ";"; }));
}

AISTUDIO_TEST(AstExtractor_Extract_EmptyContent_ReturnsRootOnly) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("");
    AISTUDIO_EXPECT(root.kind == "translation_unit");
    AISTUDIO_EXPECT(root.children.empty());
}

AISTUDIO_TEST(AstExtractor_Extract_NestedStructure_IsPreserved) {
    const AstExtractor extractor;
    const auto root = extractor.Extract("class Foo {\n    void Bar();\n};\n");
    AISTUDIO_EXPECT(CountNodes(root) > 3);
    AISTUDIO_EXPECT(AnyNode(root, [](const AstNode& n) { return n.kind == "field_declaration_list"; }));
}
