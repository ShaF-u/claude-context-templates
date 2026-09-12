#pragma once

#include "Core/Index/Symbol.hpp"

namespace aistudio::core {

// One SymbolSearch hit — docs/ROADMAP.md Phase 3 "Semantic Search" >
// "Symbol Search". `score` orders matches against each other within one
// search (see SymbolSearch::Search), not an absolute quality measure.
struct SymbolMatch {
    Symbol symbol;
    int score = 0;
};

} // namespace aistudio::core
