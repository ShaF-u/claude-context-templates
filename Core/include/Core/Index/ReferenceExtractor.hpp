#pragma once

#include "Core/Index/ReferenceEdge.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Extracts type-reference sites from a single file's source text using
// tree-sitter-cpp (same grammar SymbolExtractor/IncludeExtractor/
// CallExtractor/InheritanceExtractor use — vendored, see
// docs/DEPENDENCY_MANAGEMENT.md). Walks the whole tree for
// `type_identifier` nodes, which appear at every place a project-defined
// type name is used as a type (field/parameter/return/variable types,
// base classes, template arguments, casts, ...) — except a
// class/struct's own name at its definition or forward declaration,
// which isn't a "use" of the type and is excluded.
class ReferenceExtractor {
public:
    [[nodiscard]] std::vector<ReferenceEdge> Extract(const std::string& file_path, const std::string& content) const;
};

} // namespace aistudio::core
