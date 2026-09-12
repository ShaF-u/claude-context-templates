#pragma once

#include "Core/Index/InheritanceEdge.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Extracts base-class relationships from a single file's source text
// using tree-sitter-cpp (same grammar SymbolExtractor/IncludeExtractor/
// CallExtractor use — vendored, see docs/DEPENDENCY_MANAGEMENT.md). Only
// class/struct *definitions* (a `base_class_clause` only exists on one)
// are considered — a forward declaration has neither a body nor a base
// list, same distinction SymbolExtractor makes.
class InheritanceExtractor {
public:
    [[nodiscard]] std::vector<InheritanceEdge> Extract(const std::string& file_path, const std::string& content) const;
};

} // namespace aistudio::core
