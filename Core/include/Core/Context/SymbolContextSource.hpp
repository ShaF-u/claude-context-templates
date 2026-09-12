#pragma once

#include "Core/Context/ContextItem.hpp"
#include "Core/Index/Symbol.hpp"

namespace aistudio::core {

// Turns an indexed Symbol into a ContextItem — Context Retrieval's
// "Symbol retrieval" (docs/ROADMAP.md Phase 2), now that Phase 3's
// SymbolIndex exists to retrieve from. The content is a short synthetic
// descriptor (name/kind/location) followed by the symbol's real
// declaration header (Symbol::signature) when one was captured — e.g.
// "Function Foo::Bar (a.cpp:12)\nint Foo::Bar(int x) const". The body
// itself is never included (see Symbol::signature), so this stays
// compact even for large class/namespace definitions. Symbols
// constructed without a signature (e.g. in tests) fall back to the
// descriptor alone. Tagged CompressionLevel::Symbol — Context Compression's
// "Symbol representation" (docs/ROADMAP.md Phase 2).
[[nodiscard]] ContextItem MakeSymbolContextItem(const Symbol& symbol, int priority = 60);

} // namespace aistudio::core
