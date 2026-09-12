#pragma once

#include "Core/Context/ContextItem.hpp"

#include <cstdint>

namespace aistudio::core {

// Reduces a ContextItem's token cost when it doesn't fit as-is
// (docs/MASTER_SPEC.md #15 Context Compression). Only the Raw -> Summary
// step exists here: a head/tail excerpt with an omission marker. Ast,
// Symbol, and SemanticSummary tiers need Project Intelligence (Phase 3)
// and an LLM (Phase 4) respectively, and are deferred until those exist
// (AGENT.md #14 — minimal implementation first).
class ContextCompressor {
public:
    // Returns `item` unchanged if it already fits `target_tokens` (or if
    // there's nothing sensible to cut). Otherwise returns a Summary-level
    // copy sized to fit within `target_tokens`.
    [[nodiscard]] ContextItem Compress(ContextItem item, std::int64_t target_tokens) const;
};

} // namespace aistudio::core
