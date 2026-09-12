#include "Core/Context/ContextAuditEntry.hpp"

namespace aistudio::core {

std::string ToString(ContextAuditAction action) {
    switch (action) {
        case ContextAuditAction::Included: return "Included";
        case ContextAuditAction::Excluded: return "Excluded";
        case ContextAuditAction::Compressed: return "Compressed";
    }
    return "Unknown";
}

} // namespace aistudio::core
