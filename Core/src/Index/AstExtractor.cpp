#include "Core/Index/AstExtractor.hpp"

#include "Core/Util/Utf8.hpp"

// Vendored third-party parsing library (docs/DEPENDENCY_MANAGEMENT.md).
// Suppressed from our own /W4 (MSVC) since this header isn't ours to fix.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <tree_sitter/api.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <cstddef>
#include <cstdint>

// tree-sitter-cpp's own public entry point (see SymbolExtractor.cpp for
// why this isn't vendored separately).
extern "C" const TSLanguage* tree_sitter_cpp(void);

namespace aistudio::core {

namespace {

// Caps a single leaf node's captured text so one oversized token (a long
// string/comment literal spanning many lines) can't dominate the tree's
// memory footprint the way it would if we kept every byte verbatim.
constexpr std::size_t kMaxLeafTextLength = 120;

std::string LeafText(TSNode node, const std::string& content) {
    const auto start = ts_node_start_byte(node);
    const auto end = ts_node_end_byte(node);
    std::string text = content.substr(start, end - start);
    if (text.size() > kMaxLeafTextLength) {
        // Utf8SafeTruncationLength, not a raw resize(kMaxLeafTextLength):
        // a leaf can be a comment/string-literal token containing this
        // project's own Japanese commentary, and a naive byte-count cut
        // can land mid-character (docs/ROADMAP.md CE-5).
        text.resize(Utf8SafeTruncationLength(text, kMaxLeafTextLength));
        text += "...";
    }
    return text;
}

AstNode BuildNode(TSNode node, const std::string& content) {
    AstNode result;
    result.kind = ts_node_type(node);
    result.start_line = static_cast<int>(ts_node_start_point(node).row) + 1;
    result.end_line = static_cast<int>(ts_node_end_point(node).row) + 1;

    const uint32_t named_child_count = ts_node_named_child_count(node);
    result.children.reserve(named_child_count);
    for (uint32_t i = 0; i < named_child_count; ++i) {
        result.children.push_back(BuildNode(ts_node_named_child(node, i), content));
    }

    // Only leaves carry text — an interior node's source text is already
    // fully represented by its children, so repeating it would just
    // duplicate the whole subtree's bytes for no benefit.
    if (result.children.empty()) {
        result.text = LeafText(node, content);
    }

    return result;
}

} // namespace

AstNode AstExtractor::Extract(const std::string& content) const {
    AstNode root;

    TSParser* parser = ts_parser_new();
    if (parser == nullptr) {
        return root;
    }
    ts_parser_set_language(parser, tree_sitter_cpp());

    TSTree* tree = ts_parser_parse_string(parser, nullptr, content.c_str(), static_cast<uint32_t>(content.size()));
    if (tree != nullptr) {
        root = BuildNode(ts_tree_root_node(tree), content);
        ts_tree_delete(tree);
    }

    ts_parser_delete(parser);
    return root;
}

} // namespace aistudio::core
