#include "Core/Index/Symbol.hpp"

namespace aistudio::core {

std::string ToString(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Class: return "Class";
        case SymbolKind::Struct: return "Struct";
        case SymbolKind::Function: return "Function";
        case SymbolKind::Namespace: return "Namespace";
        case SymbolKind::Variable: return "Variable";
    }
    return "Unknown";
}

} // namespace aistudio::core
