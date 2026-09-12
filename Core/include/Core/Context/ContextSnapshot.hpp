#pragma once

#include "Core/Context/ContextSelector.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aistudio::core {

// A saved record of what was in context for a given Task/moment, so a
// developer (or the Agent) can see later what the LLM was actually given
// — docs/MASTER_SPEC.md #20 Context Snapshot. Stores item ids, not their
// content: restoring a snapshot means re-resolving those ids through
// whatever ContextSource still has them, not replaying stale content.
struct ContextSnapshot {
    std::string id;
    std::string task_name;
    std::string git_commit; // empty until Git Backend (Phase 6) exists
    std::int64_t used_tokens = 0;
    std::int64_t max_tokens = 0;
    std::vector<std::string> included_item_ids;
    // Parallel to included_item_ids (same length/order) when known --
    // each entry is the ContextSourceKind the id at the same index came
    // from, captured from ContextItem::source at snapshot time so
    // ContextRestorer can dispatch to the right source instead of
    // guessing a kind from the id's own string shape (docs/ROADMAP.md
    // "Context Restore" -- an id-shape guess turned out to have genuine
    // collisions, e.g. a Symbol id for an "operator->" overload contains
    // "->", the same substring a Dependency id's separator uses).
    // Deliberately left EMPTY for a snapshot taken before this field
    // existed (an old row read back from a pre-migration database, where
    // the new column defaults to '') -- ContextRestorer treats a length
    // mismatch against included_item_ids as "kind unknown for this
    // snapshot" and falls back to its original file-shape guess for
    // every id, rather than reading kinds[i] out of bounds or misaligned.
    std::vector<ContextSourceKind> included_item_source_kinds;
    std::int64_t created_at = 0; // unix seconds
};

// Builds a ContextSnapshot from a completed selection, stamped with the
// current time — ties ContextSelection (Context Core) to persistence
// (Context Management) without ContextSelector needing to know about
// storage.
[[nodiscard]] ContextSnapshot MakeContextSnapshot(std::string id, std::string task_name,
                                                   const ContextSelection& selection, std::int64_t max_tokens);

} // namespace aistudio::core
