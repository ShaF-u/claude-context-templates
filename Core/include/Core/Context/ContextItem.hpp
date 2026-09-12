#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aistudio::core {

// What kind of thing produced a ContextItem — mirrors the granularity
// levels docs/MASTER_SPEC.md #6 lists. More sources (Blueprint, Asset,
// Scene, ...) arrive with their own Backends in later phases; this enum
// grows alongside them rather than trying to enumerate everything now.
enum class ContextSourceKind {
    File,
    Symbol,
    Dependency,
    GitDiff,
    Custom,
};

[[nodiscard]] std::string ToString(ContextSourceKind kind);

// Inverse of ToString(ContextSourceKind) -- used when a persisted
// ContextSourceKind (e.g. ContextSnapshotRepository's per-id
// source_kind column, docs/ROADMAP.md "Context Restore") comes back off
// disk as a plain TEXT column and needs to become the enum again. An
// unrecognized string (a future kind this build doesn't know about yet,
// or a corrupted row) falls back to Custom rather than guessing File --
// Custom already means "no dedicated restoration source", the same
// meaning an unrecognized kind should have.
[[nodiscard]] ContextSourceKind ContextSourceKindFromString(const std::string& text);

// How much a ContextItem's content has been reduced from its original
// form (docs/MASTER_SPEC.md #15 Context Compression). SemanticSummary/
// Diff still need an LLM (Phase 4) and are added alongside that, not
// enumerated speculatively now. Reference exists — DependencyContextSource
// produces items at this level: a compact "file A includes file B"
// relationship, never the related file's actual content. Ast exists now
// that Phase 3's AstIndex exists to render from (AstContextSource) — a
// bounded structural outline of a file, smaller than its Raw content but
// richer than a Symbol/Reference entry. Symbol exists now that Phase 3's
// SymbolIndex exists to render from (SymbolContextSource) — a single
// declaration/signature line, smaller than an Ast outline.
enum class CompressionLevel {
    Raw,
    Summary,
    Reference,
    Ast,
    Symbol,
};

[[nodiscard]] std::string ToString(CompressionLevel level);

// A single retrievable unit of information Context Engine can decide to
// include or exclude when assembling a request for the LLM
// (docs/MASTER_SPEC.md #5 Context Engine, #14 Context Priority).
struct ContextItem {
    std::string id; // stable key, e.g. a FileMetadata::path
    ContextSourceKind source = ContextSourceKind::File;
    std::string content;
    CompressionLevel compression = CompressionLevel::Raw;

    // 0 (irrelevant) - 100 (currently being edited), the scale
    // docs/MASTER_SPEC.md #14 documents.
    int priority = 0;

    // Approximate token cost — see EstimateTokens(). Callers set this
    // before handing the item to ContextSelector.
    std::int64_t estimated_tokens = 0;

    // Other ContextItem ids this one relates to (e.g. a Symbol item
    // depending on its containing File item). Informational for now;
    // Context Compression / Garbage Collector consult it later.
    std::vector<std::string> depends_on;
};

// Rough token estimate: ~4 characters per token, the common approximation
// docs/MASTER_SPEC.md #17 uses for English-ish text. Swap for a real
// tokenizer once one is wired in (Phase 4 LLM Integration) — call sites
// depend only on this function, not the approximation.
[[nodiscard]] std::int64_t EstimateTokens(const std::string& content);

} // namespace aistudio::core
