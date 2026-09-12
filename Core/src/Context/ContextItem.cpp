#include "Core/Context/ContextItem.hpp"

namespace aistudio::core {

std::int64_t EstimateTokens(const std::string& content) {
    // Ceiling division so any non-empty content costs at least 1 token.
    return static_cast<std::int64_t>((content.size() + 3) / 4);
}

std::string ToString(ContextSourceKind kind) {
    switch (kind) {
        case ContextSourceKind::File:       return "File";
        case ContextSourceKind::Symbol:     return "Symbol";
        case ContextSourceKind::Dependency: return "Dependency";
        case ContextSourceKind::GitDiff:    return "GitDiff";
        case ContextSourceKind::Custom:     return "Custom";
    }
    return "Unknown";
}

ContextSourceKind ContextSourceKindFromString(const std::string& text) {
    if (text == "File") return ContextSourceKind::File;
    if (text == "Symbol") return ContextSourceKind::Symbol;
    if (text == "Dependency") return ContextSourceKind::Dependency;
    if (text == "GitDiff") return ContextSourceKind::GitDiff;
    return ContextSourceKind::Custom; // "Custom" itself, and any unrecognized value.
}

std::string ToString(CompressionLevel level) {
    switch (level) {
        case CompressionLevel::Raw:       return "Raw";
        case CompressionLevel::Summary:   return "Summary";
        case CompressionLevel::Reference: return "Reference";
        case CompressionLevel::Ast:       return "Ast";
        case CompressionLevel::Symbol:    return "Symbol";
    }
    return "Unknown";
}

} // namespace aistudio::core
