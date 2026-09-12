#pragma once

#include "Core/Index/IncludeEdge.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Extracts `#include` directives from a single file's source text using
// the same tree-sitter-cpp grammar SymbolExtractor uses (vendored — see
// docs/DEPENDENCY_MANAGEMENT.md). `resolved_path` on every returned edge
// is always empty — resolving an include's text to a project file
// requires knowing the whole project's file list, which only
// IncludeGraph has; this class only sees one file at a time.
class IncludeExtractor {
public:
    [[nodiscard]] std::vector<IncludeEdge> Extract(const std::string& file_path, const std::string& content) const;
};

} // namespace aistudio::core
