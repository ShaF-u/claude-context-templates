#pragma once

#include "Core/Context/ContextItem.hpp"
#include "Core/Index/AstNode.hpp"

#include <string>

namespace aistudio::core {

// Turns an indexed AstNode tree into a ContextItem — Context Compression's
// "AST representation" (docs/ROADMAP.md Phase 2), now that Phase 3's
// AstIndex exists to render from. The content is an indented structural
// outline (node kind, leaf text, 1-based line range per node), bounded in
// both depth and total node count so it stays a genuine compression of a
// file's Raw content instead of just reformatting the whole parse tree —
// a large/deeply-nested subtree is cut off with a "truncated"/"more"
// marker rather than fully expanded. CompressionLevel::Ast, distinct from
// Reference (DependencyContextSource) and Summary (ContextCompressor):
// this preserves a file's actual structure, not just a relationship or a
// head/tail excerpt of its raw text.
[[nodiscard]] ContextItem MakeAstContextItem(const std::string& file_path, const AstNode& root, int priority = 55);

} // namespace aistudio::core
