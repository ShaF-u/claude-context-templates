#include "test_framework.hpp"
#include "Core/Index/IncludeExtractor.hpp"

#include <algorithm>

using namespace aistudio::core;

namespace {
const IncludeEdge* Find(const std::vector<IncludeEdge>& edges, const std::string& include_text) {
    const auto it = std::find_if(edges.begin(), edges.end(),
                                  [&](const IncludeEdge& e) { return e.include_text == include_text; });
    return it == edges.end() ? nullptr : &*it;
}
} // namespace

AISTUDIO_TEST(IncludeExtractor_ExtractsQuotedInclude) {
    const IncludeExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "#include \"Core/Index/Symbol.hpp\"\n");
    const auto* edge = Find(edges, "Core/Index/Symbol.hpp");
    AISTUDIO_EXPECT(edge != nullptr);
    AISTUDIO_EXPECT(!edge->is_system);
    AISTUDIO_EXPECT(edge->from_file == "a.cpp");
    AISTUDIO_EXPECT(edge->resolved_path.empty());
}

AISTUDIO_TEST(IncludeExtractor_ExtractsSystemInclude) {
    const IncludeExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "#include <string>\n");
    const auto* edge = Find(edges, "string");
    AISTUDIO_EXPECT(edge != nullptr);
    AISTUDIO_EXPECT(edge->is_system);
}

AISTUDIO_TEST(IncludeExtractor_ExtractsMultipleIncludes) {
    const IncludeExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "#include <string>\n#include \"Foo.hpp\"\n#include <vector>\n");
    AISTUDIO_EXPECT(edges.size() == 3);
}

AISTUDIO_TEST(IncludeExtractor_RecordsOneBasedLineNumber) {
    const IncludeExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "// comment\n#include \"Foo.hpp\"\n");
    const auto* edge = Find(edges, "Foo.hpp");
    AISTUDIO_EXPECT(edge != nullptr);
    AISTUDIO_EXPECT(edge->line == 2);
}

AISTUDIO_TEST(IncludeExtractor_NoIncludes_ReturnsEmpty) {
    const IncludeExtractor extractor;
    const auto edges = extractor.Extract("a.cpp", "int main() { return 0; }\n");
    AISTUDIO_EXPECT(edges.empty());
}
