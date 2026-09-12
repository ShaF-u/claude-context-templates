#include "test_framework.hpp"
#include "Core/Index/ReferenceExtractor.hpp"

#include <algorithm>

using namespace aistudio::core;

namespace {
bool Contains(const std::vector<ReferenceEdge>& edges, const std::string& type_name) {
    return std::any_of(edges.begin(), edges.end(), [&](const ReferenceEdge& e) { return e.type_name == type_name; });
}

std::size_t Count(const std::vector<ReferenceEdge>& edges, const std::string& type_name) {
    return static_cast<std::size_t>(
        std::count_if(edges.begin(), edges.end(), [&](const ReferenceEdge& e) { return e.type_name == type_name; }));
}
} // namespace

AISTUDIO_TEST(ReferenceExtractor_ExtractsFieldType) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo {\n    Bar b;\n};\n");
    AISTUDIO_EXPECT(Contains(edges, "Bar"));
}

AISTUDIO_TEST(ReferenceExtractor_ClassOwnName_IsNotIndexedAsReference) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo {\n};\n");
    AISTUDIO_EXPECT(!Contains(edges, "Foo"));
}

AISTUDIO_TEST(ReferenceExtractor_ForwardDeclarationOwnName_IsNotIndexedAsReference) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo;\n");
    AISTUDIO_EXPECT(!Contains(edges, "Foo"));
}

AISTUDIO_TEST(ReferenceExtractor_ExtractsBaseClassReference) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.hpp", "class Foo : public Bar {\n};\n");
    AISTUDIO_EXPECT(Contains(edges, "Bar"));
    AISTUDIO_EXPECT(!Contains(edges, "Foo"));
}

AISTUDIO_TEST(ReferenceExtractor_ExtractsParameterType) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo(Bar b) {\n}\n");
    AISTUDIO_EXPECT(Contains(edges, "Bar"));
}

AISTUDIO_TEST(ReferenceExtractor_ExtractsReturnType) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "Bar Foo() {\n}\n");
    AISTUDIO_EXPECT(Contains(edges, "Bar"));
}

AISTUDIO_TEST(ReferenceExtractor_ExtractsLocalVariableType) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo() {\n    Bar b;\n}\n");
    AISTUDIO_EXPECT(Contains(edges, "Bar"));
}

AISTUDIO_TEST(ReferenceExtractor_MultipleUsesOfSameType_AllRecorded) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo(Bar a, Bar b) {\n}\n");
    AISTUDIO_EXPECT(Count(edges, "Bar") == 2);
}

AISTUDIO_TEST(ReferenceExtractor_PrimitiveTypes_AreNotIndexed) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo(int x) {\n}\n");
    AISTUDIO_EXPECT(edges.empty());
}

AISTUDIO_TEST(ReferenceExtractor_RecordsOneBasedLineNumber) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "// comment\nvoid Foo(Bar b) {\n}\n");
    const auto it = std::find_if(edges.begin(), edges.end(), [](const ReferenceEdge& e) { return e.type_name == "Bar"; });
    AISTUDIO_EXPECT(it != edges.end());
    AISTUDIO_EXPECT(it->line == 2);
}

AISTUDIO_TEST(ReferenceExtractor_NoTypeReferences_ReturnsEmpty) {
    const ReferenceExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "int x = 1;\n");
    AISTUDIO_EXPECT(edges.empty());
}
