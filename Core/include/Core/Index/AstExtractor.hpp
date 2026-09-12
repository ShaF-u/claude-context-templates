#pragma once

#include "Core/Index/AstNode.hpp"

#include <string>

namespace aistudio::core {

// Converts a source file's tree-sitter-cpp parse into an owned AstNode
// tree (docs/ROADMAP.md Phase 3 "AST") — the mechanical, per-construct-
// kind-agnostic counterpart to SymbolExtractor's semantic declaration
// extraction. Walks every named node (tree-sitter's own AST-vs-CST
// distinction — ts_node_is_named()), so the result is a faithful,
// generic AST rather than a hand-picked subset of node kinds.
class AstExtractor {
public:
    [[nodiscard]] AstNode Extract(const std::string& content) const;
};

} // namespace aistudio::core
