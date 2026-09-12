#pragma once

#include <string>

namespace aistudio::core {

// One usage site of a type name — docs/ROADMAP.md Phase 3 "Code
// Intelligence" > "Reference Graph". Deliberately scoped to *type*
// references (a variable's declared type, a parameter/return type, a
// base class, a template argument, a cast target, ...) rather than
// every possible identifier reference (a variable *read*/*write*, a
// function call — CallGraph already covers calls, and general
// variable-usage tracking needs real data-flow analysis this project
// doesn't have). `type_name` is the plain identifier text tree-sitter-
// cpp's `type_identifier` node gives — no namespace qualification is
// resolved beyond what's directly written, same limitation
// CallEdge::callee_text and InheritanceEdge::base_name already
// document for their own equivalents.
struct ReferenceEdge {
    std::string type_name;
    std::string referencing_file; // relative, matches FileMetadata::path
    int line = 0;                 // 1-based
};

} // namespace aistudio::core
