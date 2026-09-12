#pragma once

#include "Core/Index/CallEdge.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Extracts call sites from a single file's source text using
// tree-sitter-cpp (same grammar SymbolExtractor/IncludeExtractor use —
// vendored, see docs/DEPENDENCY_MANAGEMENT.md). A call site is only
// recorded while inside a function definition's body — top-level calls
// (static initializers, macro invocations at namespace scope) have no
// meaningful "caller" and are skipped. No type resolution is attempted:
// `CallEdge::callee_text` is whatever text the call expression's callee
// spans, nothing more (mirrors SymbolExtractor's own documented limits).
class CallExtractor {
public:
    [[nodiscard]] std::vector<CallEdge> Extract(const std::string& file_path, const std::string& content) const;
};

} // namespace aistudio::core
