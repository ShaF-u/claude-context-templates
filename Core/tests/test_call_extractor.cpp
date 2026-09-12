#include "test_framework.hpp"
#include "Core/Index/CallExtractor.hpp"

#include <algorithm>

using namespace aistudio::core;

namespace {
const CallEdge* Find(const std::vector<CallEdge>& edges, const std::string& callee_text) {
    const auto it = std::find_if(edges.begin(), edges.end(),
                                  [&](const CallEdge& e) { return e.callee_text == callee_text; });
    return it == edges.end() ? nullptr : &*it;
}
} // namespace

AISTUDIO_TEST(CallExtractor_ExtractsFreeFunctionCall) {
    const CallExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo() {\n    Bar();\n}\n");
    const auto* edge = Find(edges, "Bar");
    AISTUDIO_EXPECT(edge != nullptr);
    AISTUDIO_EXPECT(edge->caller_name == "Foo");
    AISTUDIO_EXPECT(edge->caller_file == "a.cpp");
}

AISTUDIO_TEST(CallExtractor_ExtractsMemberCall) {
    const CallExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo() {\n    selector.Select();\n}\n");
    const auto* edge = Find(edges, "selector.Select");
    AISTUDIO_EXPECT(edge != nullptr);
    AISTUDIO_EXPECT(edge->caller_name == "Foo");
}

AISTUDIO_TEST(CallExtractor_ExtractsQualifiedCall) {
    const CallExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo() {\n    Ns::Bar();\n}\n");
    const auto* edge = Find(edges, "Ns::Bar");
    AISTUDIO_EXPECT(edge != nullptr);
}

AISTUDIO_TEST(CallExtractor_UsesQualifiedCallerName) {
    const CallExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo::Bar() {\n    Baz();\n}\n");
    const auto* edge = Find(edges, "Baz");
    AISTUDIO_EXPECT(edge != nullptr);
    AISTUDIO_EXPECT(edge->caller_name == "Foo::Bar");
}

AISTUDIO_TEST(CallExtractor_MultipleCallsInOneFunction_AllRecorded) {
    const CallExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo() {\n    Bar();\n    Baz();\n}\n");
    AISTUDIO_EXPECT(edges.size() == 2);
    AISTUDIO_EXPECT(Find(edges, "Bar") != nullptr);
    AISTUDIO_EXPECT(Find(edges, "Baz") != nullptr);
}

AISTUDIO_TEST(CallExtractor_CallOutsideAnyFunction_IsNotRecorded) {
    const CallExtractor extractor;
    // Top-level call with no enclosing function definition (e.g. a
    // default member initializer) has no meaningful "caller" and is
    // skipped, not attributed to an empty/invented name.
    const auto edges = extractor.Extract("a.cpp", "int x = Bar();\n");
    AISTUDIO_EXPECT(edges.empty());
}

AISTUDIO_TEST(CallExtractor_RecordsOneBasedLineNumber) {
    const CallExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo() {\n    // comment\n    Bar();\n}\n");
    const auto* edge = Find(edges, "Bar");
    AISTUDIO_EXPECT(edge != nullptr);
    AISTUDIO_EXPECT(edge->line == 3);
}

AISTUDIO_TEST(CallExtractor_NoCalls_ReturnsEmpty) {
    const CallExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "void Foo() {\n}\n");
    AISTUDIO_EXPECT(edges.empty());
}
