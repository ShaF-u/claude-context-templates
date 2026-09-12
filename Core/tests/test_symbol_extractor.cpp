#include "test_framework.hpp"
#include "Core/Index/SymbolExtractor.hpp"

#include <algorithm>

using namespace aistudio::core;

namespace {
bool Contains(const std::vector<Symbol>& symbols, const std::string& name, SymbolKind kind) {
    return std::any_of(symbols.begin(), symbols.end(),
                        [&](const Symbol& s) { return s.name == name && s.kind == kind; });
}

const Symbol* Find(const std::vector<Symbol>& symbols, const std::string& name) {
    const auto it = std::find_if(symbols.begin(), symbols.end(), [&](const Symbol& s) { return s.name == name; });
    return it == symbols.end() ? nullptr : &*it;
}
} // namespace

AISTUDIO_TEST(SymbolExtractor_ExtractsNamespace) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "namespace aistudio::core {\n}\n");
    AISTUDIO_EXPECT(Contains(symbols, "aistudio::core", SymbolKind::Namespace));
}

AISTUDIO_TEST(SymbolExtractor_ExtractsClass) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class FileScanner {\npublic:\n};\n");
    AISTUDIO_EXPECT(Contains(symbols, "FileScanner", SymbolKind::Class));
}

AISTUDIO_TEST(SymbolExtractor_ExtractsStruct) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "struct FileMetadata {\n    std::string path;\n};\n");
    AISTUDIO_EXPECT(Contains(symbols, "FileMetadata", SymbolKind::Struct));
}

AISTUDIO_TEST(SymbolExtractor_ExtractsMemberFunctionDefinition) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract(
        "a.cpp", "ContextSelection ContextSelector::Select(std::vector<ContextItem> candidates) const {\n}\n");
    AISTUDIO_EXPECT(Contains(symbols, "ContextSelector::Select", SymbolKind::Function));
}

AISTUDIO_TEST(SymbolExtractor_ExtractsFreeFunctionDefinition) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "std::string ToString(SymbolKind kind) {\n}\n");
    AISTUDIO_EXPECT(Contains(symbols, "ToString", SymbolKind::Function));
}

AISTUDIO_TEST(SymbolExtractor_DoesNotFalsePositiveOnSingleTokenControlFlow) {
    const SymbolExtractor extractor;
    // "if (...)"/"for (...)"/"while (...)" only have one identifier before
    // the paren, so they never match the two-identifier function shape.
    const auto symbols = extractor.Extract(
        "a.cpp", "if (x) {\n}\nfor (int i = 0; i < 10; ++i) {\n}\nwhile (true) {\n}\n");
    AISTUDIO_EXPECT(symbols.empty());
}

AISTUDIO_TEST(SymbolExtractor_DoesNotFalsePositiveOnElseIf) {
    const SymbolExtractor extractor;
    // "else if (...)" has two identifiers ("else", "if") before the
    // paren, matching the function shape structurally — the keyword
    // blocklist is what excludes it.
    const auto symbols = extractor.Extract("a.cpp", "else if (y) {\n}\n");
    AISTUDIO_EXPECT(symbols.empty());
}

AISTUDIO_TEST(SymbolExtractor_RecordsOneBasedLineNumber) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "// comment\n// comment\nclass Foo {\n};\n");
    const auto it = std::find_if(symbols.begin(), symbols.end(), [](const Symbol& s) { return s.name == "Foo"; });
    AISTUDIO_EXPECT(it != symbols.end());
    AISTUDIO_EXPECT(it->line == 3);
}

AISTUDIO_TEST(SymbolExtractor_ForwardDeclaration_IsNotIndexed) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo;\n");
    AISTUDIO_EXPECT(!Contains(symbols, "Foo", SymbolKind::Class));
}

AISTUDIO_TEST(SymbolExtractor_CapturesFunctionSignature) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "int Foo::Bar(int x) const {\n    return x;\n}\n");
    const auto* symbol = Find(symbols, "Foo::Bar");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->signature == "int Foo::Bar(int x) const");
}

AISTUDIO_TEST(SymbolExtractor_CapturesClassSignature_ExcludingBody) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo : public Bar {\npublic:\n    int x;\n};\n");
    const auto* symbol = Find(symbols, "Foo");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->signature == "class Foo : public Bar");
    AISTUDIO_EXPECT(symbol->signature.find("int x") == std::string::npos);
}

AISTUDIO_TEST(SymbolExtractor_CapturesNamespaceSignature) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "namespace aistudio::core {\nint x;\n}\n");
    const auto* symbol = Find(symbols, "aistudio::core");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->signature == "namespace aistudio::core");
}

AISTUDIO_TEST(SymbolExtractor_ExtractsMemberVariable_QualifiedWithClassName) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo {\npublic:\n    int x;\n};\n");
    AISTUDIO_EXPECT(Contains(symbols, "Foo::x", SymbolKind::Variable));
}

AISTUDIO_TEST(SymbolExtractor_MemberVariable_CapturesSignatureWithDefaultValue) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo {\n    static const int kLimit = 5;\n};\n");
    const auto* symbol = Find(symbols, "Foo::kLimit");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->signature == "static const int kLimit = 5");
}

AISTUDIO_TEST(SymbolExtractor_MemberFunctionPrototype_IsNotIndexedAsVariable) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo {\npublic:\n    void Bar();\n};\n");
    AISTUDIO_EXPECT(!Contains(symbols, "Foo::Bar", SymbolKind::Variable));
}

AISTUDIO_TEST(SymbolExtractor_ExtractsGlobalVariable) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "int global_count = 0;\n");
    AISTUDIO_EXPECT(Contains(symbols, "global_count", SymbolKind::Variable));
}

AISTUDIO_TEST(SymbolExtractor_ExtractsNamespaceScopeVariable) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "namespace ns {\nint counter = 0;\n}\n");
    AISTUDIO_EXPECT(Contains(symbols, "counter", SymbolKind::Variable));
}

AISTUDIO_TEST(SymbolExtractor_GlobalFunctionPrototype_IsNotIndexedAsVariable) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "void Foo();\n");
    AISTUDIO_EXPECT(!Contains(symbols, "Foo", SymbolKind::Variable));
}

AISTUDIO_TEST(SymbolExtractor_LocalVariableInsideFunction_IsNotIndexed) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "void Foo() {\n    int local = 3;\n}\n");
    AISTUDIO_EXPECT(!Contains(symbols, "local", SymbolKind::Variable));
}

AISTUDIO_TEST(SymbolExtractor_ForLoopInitializer_IsNotIndexed) {
    // Regression test: a for-loop's initializer declaration sits at
    // whatever scope the loop itself is in (here, global scope, i.e.
    // outside any function) — it must not be misclassified as a
    // global-scope variable just because it isn't inside a function body.
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "void Foo() {\n    for (int i = 0; i < 10; ++i) {\n    }\n}\n");
    AISTUDIO_EXPECT(!Contains(symbols, "i", SymbolKind::Variable));
}

AISTUDIO_TEST(SymbolExtractor_LocalVariable_DoesNotFalselyMatchAsGlobal_WhenFunctionIsAtTranslationUnitScope) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract(
        "a.cpp", "void Foo() {\n    int local = 1;\n    if (local) {\n        int nested = 2;\n    }\n}\n");
    AISTUDIO_EXPECT(!Contains(symbols, "local", SymbolKind::Variable));
    AISTUDIO_EXPECT(!Contains(symbols, "nested", SymbolKind::Variable));
}

// Symbol::type_name (docs/ROADMAP.md "## LSP" Type entry) -- tree-sitter's
// own "type" field, read structurally, not text re-derived from
// `signature`. See Symbol.hpp's own doc comment for the full rationale.

AISTUDIO_TEST(SymbolExtractor_FunctionReturnType_CapturesTypeIdentifier) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "Foo Bar() {\n    return Foo();\n}\n");
    const auto* symbol = Find(symbols, "Bar");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->type_name == "Foo");
}

AISTUDIO_TEST(SymbolExtractor_FunctionReturnType_Void_TypeNameIsEmpty) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "void Foo() {\n}\n");
    const auto* symbol = Find(symbols, "Foo");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->type_name.empty());
}

AISTUDIO_TEST(SymbolExtractor_MemberVariable_CapturesTypeIdentifier) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo {\npublic:\n    Bar x;\n};\n");
    const auto* symbol = Find(symbols, "Foo::x");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->type_name == "Bar");
}

AISTUDIO_TEST(SymbolExtractor_MemberVariable_PointerToType_StillCapturesUnderlyingTypeIdentifier) {
    // The pointer wraps the declarator, not the "type" field itself --
    // tree-sitter-cpp keeps "Bar" as the plain type_identifier either way.
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo {\npublic:\n    Bar* x;\n};\n");
    const auto* symbol = Find(symbols, "Foo::x");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->type_name == "Bar");
}

AISTUDIO_TEST(SymbolExtractor_MemberVariable_PrimitiveType_TypeNameIsEmpty) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo {\npublic:\n    int x;\n};\n");
    const auto* symbol = Find(symbols, "Foo::x");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->type_name.empty());
}

AISTUDIO_TEST(SymbolExtractor_MemberVariable_QualifiedType_TypeNameIsEmpty) {
    // "std::string" parses as qualified_identifier, not type_identifier --
    // deliberately not resolved (see Symbol.hpp's own comment).
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.hpp", "class Foo {\npublic:\n    std::string x;\n};\n");
    const auto* symbol = Find(symbols, "Foo::x");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->type_name.empty());
}

AISTUDIO_TEST(SymbolExtractor_GlobalVariable_CapturesTypeIdentifier) {
    const SymbolExtractor extractor;
    const auto symbols = extractor.Extract("a.cpp", "Bar global_thing;\n");
    const auto* symbol = Find(symbols, "global_thing");
    AISTUDIO_EXPECT(symbol != nullptr);
    AISTUDIO_EXPECT(symbol->type_name == "Bar");
}
