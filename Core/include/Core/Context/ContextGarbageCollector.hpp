#pragma once

#include "Core/Database/ContextSnapshotRepository.hpp"
#include "Core/Error/Result.hpp"

#include <cstddef>

namespace aistudio::core {

// Prunes old ContextSnapshot rows so context_snapshots doesn't grow
// unbounded — docs/MASTER_SPEC.md Context Management "Context Garbage
// Collector". A snapshot is cheap to lose: ContextRestorer already
// treats a snapshot's included_item_ids as best-effort, re-resolving
// live state rather than replaying stored content (see its own class
// comment), so there's no "this snapshot is the only copy of something"
// case to protect against yet. That makes a simple "keep the N most
// recent" retention policy enough for this pass (AGENT.md #14 "最小実装
// 優先") rather than anything usage-aware (e.g. "never delete a snapshot
// some Task still references" — no such reference exists yet).
class ContextGarbageCollector {
public:
    struct Options {
        // Snapshots beyond the `max_snapshots_to_keep` most recent (by
        // created_at, ties broken by insertion order) are removed.
        std::size_t max_snapshots_to_keep = 100;
    };

    explicit ContextGarbageCollector(ContextSnapshotRepository& repository, Options options = {})
        : repository_(repository), options_(options) {}

    // Removes every snapshot beyond the retention policy in one pass.
    // Returns the number actually removed (0 if already within the
    // limit). Fails only if the underlying repository call itself fails
    // (e.g. a FindAll()/Remove() DB error) — a partial removal before
    // such a failure is possible and not rolled back, the same
    // best-effort semantics Remove() itself already has.
    [[nodiscard]] Result<std::size_t> CollectGarbage() const;

private:
    ContextSnapshotRepository& repository_;
    Options options_;
};

} // namespace aistudio::core
