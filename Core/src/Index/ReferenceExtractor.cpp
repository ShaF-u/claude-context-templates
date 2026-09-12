#include "Core/Index/ReferenceExtractor.hpp"

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
#include <unordered_set>

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

// A byte-offset key uniquely identifies a specific node's position in
// the source (tree-sitter nodes aren't otherwise comparable/hashable
// across separate lookups) — used to mark a class/struct's own name
// node so the later generic `type_identifier` walk can recognize and
// skip that exact node instead of double-counting it as a reference to
// itself.
void WalkNode(TSNode node, const std::string& file_path, const std::string& content,
              std::unordered_set<uint32_t>& definition_name_starts, std::vector<ReferenceEdge>& edges) {
    const std::string_view type(ts_node_type(node));
    const int line = static_cast<int>(ts_node_start_point(node).row) + 1;

    if (type == "class_specifier" || type == "struct_specifier") {
        // Covers both definitions and forward declarations ("class
        // Foo;") — either way, a class/struct's own name isn't a *use*
        // of the type. This runs before the recursive descent below, so
        // the entry exists by the time that descent reaches this exact
        // name node.
        const TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        if (!ts_node_is_null(name_node)) {
            definition_name_starts.insert(ts_node_start_byte(name_node));
        }
    } else if (type == "type_identifier") {
        if (definition_name_starts.find(ts_node_start_byte(node)) == definition_name_starts.end()) {
            ReferenceEdge edge;
            edge.type_name = NodeText(node, content);
            edge.referencing_file = file_path;
            edge.line = line;
            edges.push_back(std::move(edge));
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        WalkNode(ts_node_child(node, i), file_path, content, definition_name_starts, edges);
    }
}

} // namespace

std::vector<ReferenceEdge> ReferenceExtractor::Extract(const std::string& file_path, const std::string& content) const {
    std::vector<ReferenceEdge> edges;

    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        return edges;
    }
    ts_parser_set_language(parser, tree_sitter_cpp());

    TSTree* tree = ts_parser_parse_string(parser, nullptr, content.c_str(), static_cast<uint32_t>(content.size()));
    if (tree != nullptr) {
        std::unordered_set<uint32_t> definition_name_starts;
        WalkNode(ts_tree_root_node(tree), file_path, content, definition_name_starts, edges);
        ts_tree_delete(tree);
    }

    ts_parser_delete(parser);
    return edges;
}

} // namespace aistudio::core
