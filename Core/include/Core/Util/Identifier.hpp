#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace aistudio::core {

// Whether `c` is a valid C++ identifier character (letter/digit/
// underscore). Exposed alongside TrailingIdentifier/IdentifierAt so
// callers doing their own boundary-scanning (e.g. Core/LSP/LspServer's
// "is there a `Class::` qualifier right before this identifier" check)
// don't reimplement the same predicate a third time.
[[nodiscard]] bool IsIdentifierChar(char c);

// The trailing identifier run in a piece of C++ expression text — e.g.
// "Select" from "selector.Select", "Bar" from "Foo::Bar", "ToString"
// from "ToString". Trailing non-identifier characters are skipped first,
// so "fnptr" is still found inside "(*fnptr)"; only text with no
// identifier characters at all (e.g. "()") yields an empty result.
// Shared by
// CallGraph::Callers and ImpactAnalyzer rather than each reimplementing
// the same heuristic name-matching logic (same reuse rationale as
// Core/Util/Glob.hpp).
[[nodiscard]] std::string TrailingIdentifier(const std::string& text);

// The identifier run touching byte offset `byte_offset` within `text` —
// e.g. IdentifierAt("SymbolIndex", 3) == "SymbolIndex" (offset lands
// inside the run), IdentifierAt("foo bar", 3) == "foo" (offset lands
// exactly after "foo", the common case when an editor's cursor sits
// immediately after a just-typed/clicked identifier). Falls back to
// TrailingIdentifier(text.substr(0, byte_offset)) whenever `byte_offset`
// itself isn't on an identifier character, so a cursor resting on
// whitespace/punctuation still resolves to whatever identifier
// immediately precedes it. Returns an empty string if neither applies
// (e.g. offset 0 with no identifier immediately before it). Used by
// Core/LSP/LspServer's textDocument/definition to turn an LSP cursor
// Position into the word to look up in SymbolIndex.
[[nodiscard]] std::string IdentifierAt(const std::string& text, std::size_t byte_offset);

// The [start, end) BYTE range of the identifier run IdentifierAt(text,
// byte_offset) would return the text of — same rules, same fallback to
// the trailing run before `byte_offset`, same clamping of an
// out-of-range `byte_offset` to text.size(); returns nullopt in exactly
// the cases IdentifierAt returns an empty string. Added for
// Core/LSP/LspServer's textDocument/rename and textDocument/prepareRename
// (Core/LSP/LspServer.cpp), which need the occurrence's own start/end
// byte offsets to build an LSP Range — unlike textDocument/definition's
// existing use of IdentifierAt, which only ever needed the matched
// *text* to look up in SymbolIndex, never the range it came from.
// IdentifierAt itself is now expressed in terms of this function rather
// than duplicating the same start/end scan, so the two can never
// disagree on what counts as "the identifier run" at a given offset.
[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> IdentifierRangeAt(const std::string& text,
                                                                                    std::size_t byte_offset);

} // namespace aistudio::core
