#include "Core/Search/SymbolSearch.hpp"

#include "Core/Util/Identifier.hpp"

#include <algorithm>
#include <cctype>

namespace aistudio::core {

namespace {

std::string ToLower(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(),
                    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

// Tiered relevance: an exact name (full or trailing-identifier) match
// ranks above a prefix match, which ranks above a plain substring match;
// a case-sensitive exact match gets a small bonus over a same-score
// case-insensitive one so e.g. querying "Foo" ranks a symbol literally
// named "Foo" above one named "foo" (if both existed).
int ScoreSymbol(const Symbol& symbol, const std::string& query, const std::string& lower_query) {
    const std::string short_name = TrailingIdentifier(symbol.name);
    const std::string lower_name = ToLower(symbol.name);
    const std::string lower_short = ToLower(short_name);

    int score = 0;
    if (lower_name == lower_query || lower_short == lower_query) {
        score = 100;
    } else if (lower_name.rfind(lower_query, 0) == 0 || lower_short.rfind(lower_query, 0) == 0) {
        score = 70;
    } else if (lower_name.find(lower_query) != std::string::npos) {
        score = 40;
    } else {
        return 0;
    }

    if (symbol.name == query) {
        score += 5;
    }
    return score;
}

} // namespace

std::vector<SymbolMatch> SymbolSearch::Search(const SymbolIndex& index, const std::string& query,
                                               std::size_t max_results) const {
    std::vector<SymbolMatch> matches;
    if (query.empty()) {
        return matches;
    }

    const std::string lower_query = ToLower(query);
    for (const auto& symbol : index.All()) {
        const int score = ScoreSymbol(symbol, query, lower_query);
        if (score == 0) {
            continue;
        }
        matches.push_back(SymbolMatch{symbol, score});
    }

    std::stable_sort(matches.begin(), matches.end(),
                      [](const SymbolMatch& a, const SymbolMatch& b) { return a.score > b.score; });
    if (matches.size() > max_results) {
        matches.resize(max_results);
    }

    return matches;
}

} // namespace aistudio::core
