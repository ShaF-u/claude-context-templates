#pragma once

#include <string>

namespace aistudio::core {

// One call site found inside a function's body — docs/ROADMAP.md Phase 3
// "Code Intelligence" > "Caller / Callee Graph". `caller_name` uses the
// same qualified-name convention SymbolExtractor gives a function
// definition (e.g. "ContextSelector::Select"), so edges naturally
// cross-reference SymbolIndex entries without CallGraph depending on it.
// `callee_text` is the call's callee expression exactly as written (e.g.
// "selector.Select", "Foo::Bar", "ToString", "this->DoThing") — no type
// resolution is attempted, so a member call and a free function that
// happen to share a name are indistinguishable from this text alone
// (see CallGraph::Callers).
struct CallEdge {
    std::string caller_name;
    std::string caller_file; // relative, matches FileMetadata::path
    int line = 0;            // 1-based
    std::string callee_text;
};

} // namespace aistudio::core
