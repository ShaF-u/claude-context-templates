#include "Core/Index/InheritanceExtractor.hpp"

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

// A base_class_clause's direct children alternate between
// `access_specifier` (public/private/protected — absent when the
// program relies on the class/struct default) and the base's type,
// which is either a plain `type_identifier`/`qualified_identifier` or,
// for a templated base (e.g. "Baz<int>"), a `template_type` whose own
// "name" field is the `type_identifier` — verified against tree-sitter-
// cpp's actual parse tree for "class Foo : public Bar, private
// Baz<int> {}" rather than assumed from the grammar source.
void CollectBaseNames(TSNode base_class_clause, const std::string& content, std::vector<std::string>& base_names) {
    const uint32_t child_count = ts_node_child_count(base_class_clause);
    for (uint32_t i = 0; i < child_count; ++i) {
        const TSNode child = ts_node_child(base_class_clause, i);
        const std::string_view type(ts_node_type(child));
        if (type == "type_identifier" || type == "qualified_identifier") {
            base_names.push_back(NodeText(child, content));
        } else if (type == "template_type") {
            const TSNode name_node = ts_node_child_by_field_name(child, "name", 4);
            if (!ts_node_is_null(name_node)) {
                base_names.push_back(NodeText(name_node, content));
            }
        }
    }
}

void WalkNode(TSNode node, const std::string& file_path, const std::string& content,
              std::vector<InheritanceEdge>& edges) {
    const std::string_view type(ts_node_type(node));

    if (type == "class_specifier" || type == "struct_specifier") {
        const TSNode name_node = ts_node_child_by_field_name(node, "name", 4);
        const TSNode body_node = ts_node_child_by_field_name(node, "body", 4);
        // Forward declarations ("class Foo;") have neither a body nor a
        // base_class_clause — only definitions are real, indexable edges.
        if (!ts_node_is_null(name_node) && !ts_node_is_null(body_node)) {
            const uint32_t child_count = ts_node_child_count(node);
            for (uint32_t i = 0; i < child_count; ++i) {
                const TSNode child = ts_node_child(node, i);
                if (std::string_view(ts_node_type(child)) != "base_class_clause") {
                    continue;
                }
                std::vector<std::string> base_names;
                CollectBaseNames(child, content, base_names);
                for (auto& base_name : base_names) {
                    InheritanceEdge edge;
                    edge.derived_name = NodeText(name_node, content);
                    edge.derived_file = file_path;
                    edge.line = static_cast<int>(ts_node_start_point(node).row) + 1;
                    edge.base_name = std::move(base_name);
                    edges.push_back(std::move(edge));
                }
            }
        }
    }

    const uint32_t child_count = ts_node_child_count(node);
    for (uint32_t i = 0; i < child_count; ++i) {
        WalkNode(ts_node_child(node, i), file_path, content, edges);
    }
}

} // namespace

std::vector<InheritanceEdge> InheritanceExtractor::Extract(const std::string& file_path,
                                                             const std::string& content) const {
    std::vector<InheritanceEdge> edges;

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
