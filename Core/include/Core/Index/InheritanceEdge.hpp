#pragma once

#include <string>

namespace aistudio::core {

// One `class Derived : ... Base ...` relationship — docs/ROADMAP.md
// Phase 3 "Code Intelligence" > "Inheritance Graph". `base_name` is
// always the base's plain class/struct name with no template arguments
// (e.g. "Baz" from "class Foo : public Baz<int>") — tree-sitter-cpp
// exposes a template base's name as a distinct field, so this doesn't
// need the raw-text stripping IncludeExtractor/CallExtractor rely on for
// their own equivalents. No namespace/type resolution is attempted
// beyond that: like SymbolExtractor's own class names, `derived_name`/
// `base_name` are exactly the identifier text, so a base class that
// happens to share a name with an unrelated class elsewhere in the
// project is indistinguishable from this text alone (see
// InheritanceGraph::Derived).
struct InheritanceEdge {
    std::string derived_name;
    std::string derived_file; // relative, matches FileMetadata::path
    int line = 0;             // 1-based
    std::string base_name;
};

} // namespace aistudio::core
