#pragma once

#include "Core/Index/Symbol.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Extracts class/struct/namespace declarations and function definitions
// using a real C++ grammar (tree-sitter-cpp, vendored — see
// docs/DEPENDENCY_MANAGEMENT.md), not text heuristics. This correctly
// handles templates, attributes (`[[nodiscard]]`), multi-line
// signatures, and forward declarations (which are a different node kind
// in the grammar and are naturally excluded), unlike the regex-based
// version this replaced. Still not full semantic analysis, though: no
// type resolution, overload disambiguation, or macro expansion — a
// function name is whatever text the declarator's identifier node spans,
// nothing more. Callers needing that (Reference Graph, Caller/Callee
// Graph) still need real semantic tooling, deferred to a later pass.
class SymbolExtractor {
public:
    [[nodiscard]] std::vector<Symbol> Extract(const std::string& file_path, const std::string& content) const;
};

} // namespace aistudio::core
