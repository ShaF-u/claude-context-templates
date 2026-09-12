#include "test_framework.hpp"
#include "Core/Index/InheritanceExtractor.hpp"

#include <algorithm>

using namespace aistudio::core;

namespace {
bool Contains(const std::vector<InheritanceEdge>& edges, const std::string& derived, const std::string& base) {
    return std::any_of(edges.begin(), edges.end(),
                        [&](const InheritanceEdge& e) { return e.derived_name == derived && e.base_name == base; });
}
} // namespace

AISTUDIO_TEST(InheritanceExtractor_ExtractsSingleBase) {
    const InheritanceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo : public Bar {\n};\n");
    AISTUDIO_EXPECT(Contains(edges, "Foo", "Bar"));
}

AISTUDIO_TEST(InheritanceExtractor_ExtractsMultipleBases) {
    const InheritanceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo : public Bar, private Baz {\n};\n");
    AISTUDIO_EXPECT(edges.size() == 2);
    AISTUDIO_EXPECT(Contains(edges, "Foo", "Bar"));
    AISTUDIO_EXPECT(Contains(edges, "Foo", "Baz"));
}

AISTUDIO_TEST(InheritanceExtractor_TemplatedBase_StripsTemplateArguments) {
    const InheritanceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo : public Baz<int> {\n};\n");
    AISTUDIO_EXPECT(Contains(edges, "Foo", "Baz"));
}

AISTUDIO_TEST(InheritanceExtractor_StructInheritance_IsExtracted) {
    const InheritanceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "struct Foo : Bar {\n};\n");
    AISTUDIO_EXPECT(Contains(edges, "Foo", "Bar"));
}

AISTUDIO_TEST(InheritanceExtractor_NoBaseClause_ReturnsEmpty) {
    const InheritanceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo {\n};\n");
    AISTUDIO_EXPECT(edges.empty());
}

AISTUDIO_TEST(InheritanceExtractor_ForwardDeclaration_IsNotIndexed) {
    const InheritanceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo;\n");
    AISTUDIO_EXPECT(edges.empty());
}

AISTUDIO_TEST(InheritanceExtractor_RecordsOneBasedLineNumber) {
    const InheritanceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "// comment\nclass Foo : public Bar {\n};\n");
    AISTUDIO_EXPECT(!edges.empty());
    AISTUDIO_EXPECT(edges.front().line == 2);
}
