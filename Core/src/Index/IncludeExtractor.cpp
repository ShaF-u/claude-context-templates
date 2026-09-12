#include "Core/Index/IncludeExtractor.hpp"

// Vendored third-party parsing library (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since this header isn't ours to fix.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <tree_sitter/api.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstdint>
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

void WalkNode(TSNode node, const std::string& file_path, const std::string& content, std::vector<IncludeEdge>& edges) {
    const std::string_view type(ts_node_type(node));

    if (type == "preproc_include") {
        const TSNode path_node = ts_node_child_by_field_name(node, "path", 4);
        if (!ts_node_is_null(path_node)) {
            const std::string_view path_type(ts_node_type(path_node));
            const std::string text = NodeText(path_node, content);
            const int line = static_cast<int>(ts_node_start_point(node).row) + 1;

            // string_literal is `"..."`, system_lib_string is `<...>` — both
            // include the delimiters in their node text, so strip them.
            // Macro-valued includes (`#include MACRO_PATH`, node type
            // "identifier") aren't statically resolvable without a
            // preprocessor pass and are deliberately not indexed.
            if (path_type == "string_literal" && text.size() >= 2) {
                IncludeEdge edge;
                edge.from_file = file_path;
                edge.include_text = text.substr(1, text.size() - 2);
                edge.is_system = false;
                edge.line = line;
                edges.push_back(std::move(edge));
            } else if (path_type == "system_lib_string" && text.size() >= 2) {
                IncludeEdge edge;
                edge.from_file = file_path;
                edge.include_text = text.substr(1, text.size() - 2);
                edge.is_system = true;
                edge.line = line;
                edges.push_back(std::move(edge));
            }
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        WalkNode(ts_node_child(node, i), file_path, content, edges);
    }
}

} // namespace

std::vector<IncludeEdge> IncludeExtractor::Extract(const std::string& file_path, const std::string& content) const {
    std::vector<IncludeEdge> edges;

    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        return edges;
    }
    ts_parser_set_language(parser, tree_sitter_cpp());

    TSTree* tree = ts_parser_parse_string(parser, nullptr, content.c_str(), static_cast<uint32_t>(content.size()));
    if (tree != nullptr) {
        WalkNode(ts_tree_root_node(tree), file_path, content, edges);
        ts_tree_delete(tree);
    }

    ts_parser_delete(parser);
    return edges;
}

} // namespace aistudio::core
