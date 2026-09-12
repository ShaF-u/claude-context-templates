#include "Core/Context/SymbolContextSource.hpp"

namespace aistudio::core {

ContextItem MakeSymbolContextItem(const Symbol& symbol, int priority) {
    ContextItem item;
    item.id = symbol.file_path + ":" + symbol.name;
    item.source = ContextSourceKind::Symbol;
    item.compression = CompressionLevel::Symbol;
    item.priority = priority;
    item.content = ToString(symbol.kind) + " " + symbol.name + " (" + symbol.file_path + ":" +
                    std::to_string(symbol.line) + ")";
    if (!symbol.signature.empty()) {
        item.content += "\n" + symbol.signature;
    }
    item.estimated_tokens = EstimateTokens(item.content);
    return item;
}

} // namespace aistudio::core
