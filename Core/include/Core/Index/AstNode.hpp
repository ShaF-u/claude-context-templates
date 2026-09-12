#pragma once

#include <string>
#include <vector>

namespace aistudio::core {

// A structured, persisted, reusable representation of a source file's
// parse tree — docs/ROADMAP.md Phase 3 "Code Intelligence" > "AST".
// Until now, every tree-sitter-cpp consumer (SymbolExtractor,
// IncludeExtractor, CallExtractor, InheritanceExtractor,
// ReferenceExtractor) parsed its own TSTree and threw it away once it
// pulled out the handful of fields it needed; none of them kept the AST
// itself around as reusable data the way Symbol does for declarations.
// AstNode is that missing structured form: a plain, owned tree (no
// tree-sitter handles leak past AstExtractor) that mirrors tree-sitter's
// own "named node" view of the grammar — the same distinction tree-sitter
// itself draws between semantically meaningful nodes (declarations,
// statements, expressions, identifiers, literals, ...) and purely
// syntactic ones (punctuation, keyword tokens), which keeps the tree at
// AST granularity instead of full concrete-syntax-tree granularity.
//
// Deliberately generic (kind/text/line range/children only, no
// per-construct-kind semantics) rather than re-deriving the
// class/function/variable recognition SymbolExtractor already owns —
// AstIndex is the reusable substrate those extractors could rebuild on
// top of later (docs/ROADMAP.md Phase 2 "AST Cache"), not a replacement
// for Symbol's flat, name-indexed view. Consumers that want a compact
// outline instead of the full tree (docs/ROADMAP.md Phase 2 "Context
// Compression" > "AST representation") render a bounded summary from
// this — see AstContextSource.
struct AstNode {
    std::string kind; // tree-sitter node type, e.g. "class_specifier", "identifier"
    // The node's own source text — only populated for leaf nodes (no
    // children), truncated (see AstExtractor) so a single oversized
    // leaf (a long string/comment literal) can't blow up tree size.
    // Non-leaf nodes carry their text implicitly via their children, so
    // repeating it here would just duplicate the whole subtree's bytes.
    std::string text;
    int start_line = 0; // 1-based, inclusive
    int end_line = 0;   // 1-based, inclusive
    std::vector<AstNode> children;
};

} // namespace aistudio::core
