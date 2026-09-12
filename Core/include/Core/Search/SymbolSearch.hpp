#pragma once

#include "Core/Index/SymbolIndex.hpp"
#include "Core/Search/SymbolMatch.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aistudio::core {

// Ranked lookup over an already-built SymbolIndex — docs/ROADMAP.md
// Phase 3 "Semantic Search" > "Symbol Search". SymbolIndex::FindByName()
// already covers exact-match lookup; this adds prefix/substring matching
// with relevance ranking on top (docs/ROADMAP.md "Search ranking" for
// this search mode), for the common "I roughly remember the name" case a
// symbol picker needs. Matches against both a symbol's full name (e.g.
// "Foo::Bar") and its trailing identifier alone (TrailingIdentifier,
// e.g. "Bar") so a query for the short name still finds a qualified
// member symbol.
class SymbolSearch {
public:
    [[nodiscard]] std::vector<SymbolMatch> Search(const SymbolIndex& index, const std::string& query,
                                                    std::size_t max_results = 50) const;
};

} // namespace aistudio::core
