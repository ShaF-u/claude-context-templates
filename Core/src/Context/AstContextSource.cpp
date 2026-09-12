#include "Core/Context/AstContextSource.hpp"

#include <cstddef>

namespace aistudio::core {

namespace {

// Bounds on the rendered outline — kept small enough that this stays a
// real compression of a file's content (see AstContextSource.hpp), not a
// full re-serialization of the AST.
constexpr int kMaxDepth = 5;
constexpr std::size_t kMaxRenderedNodes = 300;

void AppendLineRange(std::string& out, const AstNode& node) {
    out += " (" + std::to_string(node.start_line);
    if (node.end_line != node.start_line) {
        out += "-" + std::to_string(node.end_line);
    }
    out += ")";
}

void RenderNode(const AstNode& node, int depth, std::string& out, std::size_t& node_budget) {
    out.append(static_cast<std::size_t>(depth) * 2, ' ');
    out += node.kind;
    if (!node.text.empty()) {
        out += " \"" + node.text + "\"";
    }
    AppendLineRange(out, node);
    out += "\n";

    if (node.children.empty()) {
        return;
    }

    if (depth >= kMaxDepth) {
        out.append(static_cast<std::size_t>(depth + 1) * 2, ' ');
        out += "... (truncated at depth " + std::to_string(kMaxDepth) + ")\n";
        return;
    }

    for (const auto& child : node.children) {
        if (node_budget == 0) {
            out.append(static_cast<std::size_t>(depth + 1) * 2, ' ');
            out += "... more\n";
            return;
        }
        --node_budget;
        RenderNode(child, depth + 1, out, node_budget);
    }
}

} // namespace

ContextItem MakeAstContextItem(const std::string& file_path, const AstNode& root, int priority) {
    ContextItem item;
    item.id = file_path;
    item.source = ContextSourceKind::File;
    item.compression = CompressionLevel::Ast;
    item.priority = priority;

    std::string out;
    std::size_t node_budget = kMaxRenderedNodes;
    RenderNode(root, 0, out, node_budget);
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }

    item.content = std::move(out);
    item.estimated_tokens = EstimateTokens(item.content);
    item.depends_on = {file_path};
    return item;
}

} // namespace aistudio::core
