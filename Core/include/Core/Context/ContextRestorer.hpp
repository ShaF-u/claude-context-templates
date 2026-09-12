#pragma once

#include "Core/Context/ContextItem.hpp"
#include "Core/Context/ContextSnapshot.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/Security/Sandbox.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Re-resolves a ContextSnapshot's saved item ids back into ContextItems
// with real, current content — docs/ROADMAP.md Phase 2 "Context Restore".
// ContextSnapshot deliberately stores only ids, not content (see its own
// class comment: "restoring a snapshot means re-resolving those ids
// through whatever ContextSource still has them, not replaying stale
// content"), so this reads whatever each id currently points to, which
// may differ from — or no longer exist as — what was there when the
// snapshot was taken.
//
// Dispatches per id on ContextSnapshot::included_item_source_kinds (the
// ContextSourceKind captured at snapshot time), NOT by guessing a kind
// from the id string's shape. An earlier version of this class tried the
// shape-guess approach ("does it open as a file? assume File; otherwise
// skip") to avoid a schema change, but a real collision surfaced once
// Symbol/Dependency support was added: a Symbol id for an operator
// overload (SymbolExtractor captures e.g. "operator->" as a symbol name)
// produces an id like "a.cpp:operator->", which contains BOTH a Symbol
// id's ':' separator AND a Dependency id's "->" separator — a shape-only
// dispatcher has no correct way to tell those apart. Persisting the real
// kind (ContextSnapshot::included_item_source_kinds) removes the
// guesswork entirely. A pre-migration snapshot (read back from a
// database that predates this field) has that vector empty rather than
// aligned with included_item_ids — Restore() detects the length mismatch
// and falls back to the old file-shape guess for every id in that
// snapshot, so old rows keep restoring exactly what they always did
// (File-shaped ids only) rather than being silently reinterpreted.
//
// Kinds handled: File (FileContextSource — bare project-relative path),
// Symbol ("path:name", SymbolContextSource, resolved via `symbol_index`),
// Dependency ("pathA->pathB", DependencyContextSource, resolved via
// `include_graph`), and Custom ids that use ContextRetriever's
// "keyword:path:line" convention (Custom is also used by Backend/Plugin
// -provided items with entirely opaque ids — e.g. ProjectRulesBackend's
// "rules/project" — those have no source this class can re-read, so a
// Custom id is only restored when it matches the keyword-search prefix;
// anything else is silently skipped, same as an unresolvable id always
// has been). GitDiff (GitBackend) has no restoration source yet and is
// always skipped. `symbol_index`/`include_graph` are nullptr-tolerant —
// nullptr means Symbol/Dependency ids can't be resolved and are skipped,
// the same graceful-degradation convention ApiServerIndexes uses.
//
// A File/Symbol/Dependency/Keyword id that no longer resolves (file
// deleted/renamed, symbol removed, include edge no longer exists) is
// skipped the same way — Restore() never fails outright just because
// part of a snapshot has gone stale.
class ContextRestorer {
public:
    struct Options {
        // Root every id is resolved relative to. Should match `firewall`'s
        // own root when one is set (same as ContextRetriever::Options) —
        // a mismatch would resolve and firewall-check against two
        // different trees.
        std::string project_root;
        // nullptr applies no filtering (unlike ContextRetriever's own
        // nullable firewall, this one is expected to normally be set —
        // restoring specifically re-surfaces content that was pruned out
        // once already, so skipping the Context Firewall guarantee
        // docs/MASTER_SPEC.md #21 requires here would be an easy way to
        // regress it for old snapshots).
        const Sandbox* firewall = nullptr;
        // Resolves Symbol-kind ids ("path:name"). nullptr skips them.
        const SymbolIndex* symbol_index = nullptr;
        // Resolves Dependency-kind ids ("pathA->pathB"). nullptr skips them.
        const IncludeGraph* include_graph = nullptr;
    };

    explicit ContextRestorer(Options options) : options_(std::move(options)) {}

    [[nodiscard]] std::vector<ContextItem> Restore(const ContextSnapshot& snapshot) const;

private:
    Options options_;
};

} // namespace aistudio::core
