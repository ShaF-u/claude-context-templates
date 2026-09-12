#include "Core/Index/SymbolExtractor.hpp"

// Vendored third-party parsing library (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since this header isn't ours to fix.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <tree_sitter/api.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cctype>
#include <cstdint>
#include <optional>
#include <string_view>

// tree-sitter-cpp's own public entry point (bindings/c/tree-sitter-cpp.h
// is a 5-line file declaring exactly this; not worth vendoring
// separately just for one declaration).
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace aistudio::core {

namespace {

std::string NodeText(TSNode node, const std::string& content) {
    const auto start = ts_node_start_byte(node);
    const auto end = ts_node_end_byte(node);
    return content.substr(start, end - start);
}

// The declaration's header — from the start of `node` up to (not
// including) `body_node` — trimmed of trailing whitespace. Deliberately
// excludes the body so a class/namespace's signature stays compact
// regardless of how large its contents are.
std::string Signature(TSNode node, TSNode body_node, const std::string& content) {
    const auto node_start = ts_node_start_byte(node);
    const auto body_start = ts_node_start_byte(body_node);
    if (body_start <= node_start) {
        return {};
    }
    std::string text = content.substr(node_start, body_start - node_start);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.pop_back();
    }
    return text;
}

// A variable's signature is its whole declaration (field_declaration or
// declaration node), trimmed of trailing whitespace and the statement's
// terminating ';' — e.g. "int x", "static const std::string y" (default
// value included, if any). Unlike Signature() above there's no "body" to
// stop at; the whole node text already stays compact since a variable
// declaration is one statement.
std::string VariableSignature(TSNode node, const std::string& content) {
    std::string text = NodeText(node, content);
    auto trim_trailing_space = [&text] {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
            text.pop_back();
        }
    };
    trim_trailing_space();
    if (!text.empty() && text.back() == ';') {
        text.pop_back();
        trim_trailing_space();
    }
    return text;
}

// tree-sitter-cpp's word-token lexer falls back to a generic "identifier"
// for a reserved word when the exact keyword token isn't valid in that
// parse state but a bare identifier is (a known tree-sitter ambiguity,
// not a real grammar rule) — e.g. a stray top-level "else if (y) {}"
// fragment parses as a function named "if" returning type "else". No
// real, compiling C++ program can name a declaration after a keyword, so
// any extracted name matching one is never a legitimate symbol.
constexpr std::string_view kReservedKeywords[] = {
    "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
    "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
    "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
    "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if",
    "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
    "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
    "switch", "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename",
    "union", "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
};

bool IsReservedKeyword(const std::string& text) {
    for (const auto& keyword : kReservedKeywords) {
        if (text == keyword) {
            return true;
        }
    }
    return false;
}

// Descends through a C++ declarator chain (pointer/reference/function/
// parenthesized declarators, and any other wrapper that exposes a
// "declarator" field) to find the innermost identifier-like node — a
// function's actual name, however many `*`/`&`/qualifiers surround it.
std::optional<TSNode> FindNameNode(TSNode declarator) {
    TSNode node = declarator;
    while (true) {
        const std::string_view type(ts_node_type(node));
        if (type == "identifier" || type == "field_identifier" || type == "qualified_identifier" ||
            type == "destructor_name" || type == "operator_name" || type == "template_function") {
            return node;
        }
        const TSNode next = ts_node_child_by_field_name(node, "declarator", 10);
        if (ts_node_is_null(next)) {
            return std::nullopt;
        }
        node = next;
    }
}

// Same declarator-chain descent as FindNameNode, except a
// "function_declarator" anywhere along the chain means this declares a
// function (a member prototype like "void Bar();", or a free function
// prototype at namespace scope), not a variable — bail out rather than
// walking through it, since function_declarator also exposes its own
// "declarator" field FindNameNode would otherwise happily follow.
std::optional<TSNode> FindVariableNameNode(TSNode declarator) {
    TSNode node = declarator;
    while (true) {
        const std::string_view type(ts_node_type(node));
        if (type == "function_declarator") {
            return std::nullopt;
        }
        if (type == "identifier" || type == "field_identifier" || type == "qualified_identifier") {
            return node;
        }
        const TSNode next = ts_node_child_by_field_name(node, "declarator", 10);
        if (ts_node_is_null(next)) {
            return std::nullopt;
        }
        node = next;
    }
}

// A "declaration" node (a namespace/global-scope variable candidate) is
// only meaningful at true file/namespace scope — its direct parent is
// "translation_unit" (global) or "declaration_list" (a namespace body).
// Anywhere else (a for-loop initializer, an if-condition, a function's
// compound_statement body, ...) it's a local declaration and deliberately
// excluded (see Symbol.hpp) — checking the parent directly is far more
// precise than threading an "inside a function" flag through the
// recursion, which would still misclassify e.g. a for-loop initializer
// sitting at global scope (not inside any function at all).
bool IsNamespaceOrGlobalScopeDeclaration(TSNode node) {
    const TSNode parent = ts_node_parent(node);
    if (ts_node_is_null(parent)) {
        return false;
    }
    const std::string_view parent_type(ts_node_type(parent));
    return parent_type == "translation_unit" || parent_type == "declaration_list";
}

// Symbol::type_name for `node` (a declaration/field_declaration/
// function_definition) — see that field's own doc comment in Symbol.hpp
// for the full reasoning. Reads tree-sitter-cpp's own "type" field
// directly, the same structural approach FindNameNode/FindVariableNameNode
// already use for the sibling "declarator" field, rather than re-parsing
// Signature()'s prose. Only a bare `type_identifier` is trusted (a name
// this project's own SymbolIndex might actually define); anything else
// (`primitive_type`, `qualified_identifier`, `template_type`,
// `placeholder_type_specifier` for `auto`, ...) is deliberately left
// empty rather than guessed at.
std::string DeclaredTypeName(TSNode node, const std::string& content) {
    const TSNode type_node = ts_node_child_by_field_name(node, "type", 4);
    if (ts_node_is_null(type_node)) {
        return {};
    }
    if (std::string_view(ts_node_type(type_node)) != "type_identifier") {
        return {};
    }
    return NodeText(type_node, content);
}

void WalkNode(TSNode node, const std::string& file_path, const std::string& content, const std::string& current_class,
              std::vector<Symbol>& symbols) {
    const std::string_view type(ts_node_type(node));
    const int line = static_cast<int>(ts_node_start_point(node).row) + 1;

    std::string class_for_children = current_class;

    if (type == "namespace_definition") {
        const TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        const TSNode body_node = ts_node_child_by_field_name(node, "body", 4);
        if (!ts_node_is_null(name_node) && !ts_node_is_null(body_node)) {
            if (auto name = NodeText(name_node, content); !IsReservedKeyword(name)) {
                Symbol symbol{std::move(name), SymbolKind::Namespace, file_path, line, Signature(node, body_node, content)};
                symbols.push_back(std::move(symbol));
            }
        }
    } else if (type == "class_specifier" || type == "struct_specifier") {
        // A forward declaration ("class Foo;") is the same node type as a
        // definition but has no "body" field — only definitions are real,
        // indexable symbols.
        const TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        const TSNode body_node = ts_node_child_by_field_name(node, "body", 4);
        if (!ts_node_is_null(name_node) && !ts_node_is_null(body_node)) {
            if (auto name = NodeText(name_node, content); !IsReservedKeyword(name)) {
                // Member variables found while descending this class's body
                // get qualified with this name (e.g. "Foo::x") — captured
                // before `name` is moved into the Symbol below.
                class_for_children = name;
                const auto kind = type == "class_specifier" ? SymbolKind::Class : SymbolKind::Struct;
                Symbol symbol{std::move(name), kind, file_path, line, Signature(node, body_node, content)};
                symbols.push_back(std::move(symbol));
            }
        }
    } else if (type == "function_definition") {
        const TSNode declarator = ts_node_child_by_field_name(node, "declarator", 10);
        const TSNode body_node = ts_node_child_by_field_name(node, "body", 4);
        if (!ts_node_is_null(declarator) && !ts_node_is_null(body_node)) {
            if (const auto name_node = FindNameNode(declarator); name_node.has_value()) {
                if (auto name = NodeText(*name_node, content); !IsReservedKeyword(name)) {
                    Symbol symbol{std::move(name), SymbolKind::Function, file_path, line,
                                   Signature(node, body_node, content), DeclaredTypeName(node, content)};
                    symbols.push_back(std::move(symbol));
                }
            }
        }
    } else if (type == "field_declaration") {
        // Only appears inside a field_declaration_list (a class/struct
        // body), so `current_class` is always set here.
        const TSNode declarator = ts_node_child_by_field_name(node, "declarator", 10);
        if (!ts_node_is_null(declarator)) {
            if (const auto name_node = FindVariableNameNode(declarator); name_node.has_value()) {
                if (auto name = NodeText(*name_node, content); !IsReservedKeyword(name)) {
                    Symbol symbol{current_class + "::" + name, SymbolKind::Variable, file_path, line,
                                   VariableSignature(node, content), DeclaredTypeName(node, content)};
                    symbols.push_back(std::move(symbol));
                }
            }
        }
    } else if (type == "declaration" && IsNamespaceOrGlobalScopeDeclaration(node)) {
        const TSNode declarator = ts_node_child_by_field_name(node, "declarator", 10);
        if (!ts_node_is_null(declarator)) {
            if (const auto name_node = FindVariableNameNode(declarator); name_node.has_value()) {
                if (auto name = NodeText(*name_node, content); !IsReservedKeyword(name)) {
                    Symbol symbol{std::move(name), SymbolKind::Variable, file_path, line,
                                   VariableSignature(node, content), DeclaredTypeName(node, content)};
                    symbols.push_back(std::move(symbol));
                }
            }
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        WalkNode(ts_node_child(node, i), file_path, content, class_for_children, symbols);
    }
}

} // namespace

std::vector<Symbol> SymbolExtractor::Extract(const std::string& file_path, const std::string& content) const {
    std::vector<Symbol> symbols;

    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        return symbols;
    }
    ts_parser_set_language(parser, tree_sitter_cpp());

    TSTree* tree = ts_parser_parse_string(parser, nullptr, content.c_str(), static_cast<uint32_t>(content.size()));
    if (tree != nullptr) {
        WalkNode(ts_tree_root_node(tree), file_path, content, "", symbols);
        ts_tree_delete(tree);
    }

    ts_parser_delete(parser);
    return symbols;
}

} // namespace aistudio::core
