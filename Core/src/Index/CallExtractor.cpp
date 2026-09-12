#include "Core/Index/CallExtractor.hpp"

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

// tree-sitter-cpp's own public entry point (see SymbolExtractor.cpp for
// why this isn't vendored separately).
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace aistudio::core {

namespace {

std::string NodeText(TSNode node, const std::string& content) {
    const auto start = ts_node_start_byte(node);
    const auto end = ts_node_end_byte(node);
    return content.substr(start, end - start);
}

// Same tree-sitter lexer-fallback ambiguity SymbolExtractor.cpp
// documents: a reserved word can surface as a plain "identifier" node in
// a position where the real keyword token wouldn't parse. Duplicated
// here rather than shared — see SymbolExtractor.cpp's own note on why a
// TSNode-based helper isn't exposed through a header.
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

// The enclosing function_definition's qualified name, in the same form
// SymbolExtractor gives it (e.g. "ContextSelector::Select"), or nullopt
// if this isn't a real function definition (no body/declarator, or a
// keyword-fallback false positive — see SymbolExtractor.cpp).
std::optional<std::string> FunctionDefinitionName(TSNode node, const std::string& content) {
    const TSNode declarator = ts_node_child_by_field_name(node, "declarator", 10);
    const TSNode body_node = ts_node_child_by_field_name(node, "body", 4);
    if (ts_node_is_null(declarator) || ts_node_is_null(body_node)) {
        return std::nullopt;
    }
    const auto name_node = FindNameNode(declarator);
    if (!name_node.has_value()) {
        return std::nullopt;
    }
    auto name = NodeText(*name_node, content);
    if (IsReservedKeyword(name)) {
        return std::nullopt;
    }
    return name;
}

// `current_caller` is threaded by value through the recursion, so each
// function_definition subtree gets its own scope without any explicit
// stack bookkeeping — a call site outside every function (empty
// current_caller) is simply not recorded (see CallExtractor.hpp).
void WalkNode(TSNode node, const std::string& file_path, const std::string& content,
              const std::string& current_caller, std::vector<CallEdge>& edges) {
    const std::string_view type(ts_node_type(node));

    std::string caller_for_children = current_caller;
    if (type == "function_definition") {
        if (const auto name = FunctionDefinitionName(node, content); name.has_value()) {
            caller_for_children = *name;
        }
    } else if (type == "call_expression" && !current_caller.empty()) {
        const TSNode function_node = ts_node_child_by_field_name(node, "function", 8);
        if (!ts_node_is_null(function_node)) {
            CallEdge edge;
            edge.caller_name = current_caller;
            edge.caller_file = file_path;
            edge.line = static_cast<int>(ts_node_start_point(node).row) + 1;
            edge.callee_text = NodeText(function_node, content);
            edges.push_back(std::move(edge));
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        WalkNode(ts_node_child(node, i), file_path, content, caller_for_children, edges);
    }
}

} // namespace

std::vector<CallEdge> CallExtractor::Extract(const std::string& file_path, const std::string& content) const {
    std::vector<CallEdge> edges;

    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        return edges;
    }
    ts_parser_set_language(parser, tree_sitter_cpp());

    TSTree* tree = ts_parser_parse_string(parser, nullptr, content.c_str(), static_cast<uint32_t>(content.size()));
    if (tree != nullptr) {
        WalkNode(ts_tree_root_node(tree), file_path, content, "", edges);
        ts_tree_delete(tree);
    }

    ts_parser_delete(parser);
    return edges;
}

} // namespace aistudio::core
