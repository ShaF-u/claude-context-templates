#pragma once

#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Context/ContextItem.hpp"
#include "Core/IDE/EditorStateStore.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/Project/FileCache.hpp"
#include "Core/Project/FileMetadata.hpp"
#include "Core/Security/Sandbox.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace aistudio::core {

// Turns a free-text intent ("Player attack", "csv parser") into a ranked
// list of ContextItems — docs/ROADMAP.md Phase 2 "Context Retrieval",
// the piece its File/Symbol/Dependency retrieval entries were missing:
// MakeFileContextItem/MakeSymbolContextItem/MakeDependencyContextItems
// each turn ONE given file/symbol/file into an item, but none of them
// SELECT which one is relevant to a query. This is that selection step,
// built entirely on infrastructure that already exists (SymbolSearch /
// KeywordSearch for matching, IncludeGraph for expansion) rather than
// needing real NLP Intent Analysis — Phase 4 "LLM Integration" doesn't
// exist yet, so this is a heuristic stand-in. The exit criteria's own
// example ("Player::Attack, AttackComponent, Weapon, 関連Animation,
// 関連Blueprint" — never "Playerを丸ごと") is written to tolerate exactly
// this: matched symbols and their direct dependencies, not whole files.
//
// Combines four signals, deduplicated by ContextItem::id, each landing
// at a different docs/MASTER_SPEC.md #14 Context Priority tier:
//   - Symbol name match (SymbolSearch)                    -> 35-85
//   - Include dependencies of a matched symbol's own file -> 35
//   - File path match (substring on FileMetadata::path)   -> 25-60
//   - Keyword/content match (KeywordSearch, fallback)      -> 15-35
// A file or keyword hit whose file is already covered by a symbol match
// is skipped — the symbol-level item is the more precise signal for
// that file, so a coarser one for the same file is just noise.
//
// Context Firewall (docs/MASTER_SPEC.md #21 — "AIからアクセスできる範囲を
// 制限してください"): when `Options::firewall` is set, a Symbol/File/
// Keyword match whose own file Sandbox::IsAllowed() rejects is dropped
// before it's ever turned into a ContextItem — it's not merely
// deprioritized, it's never considered. Dependency items are generated
// from a matched symbol's (already firewall-checked) file, but each
// related_path the edge names is independently firewall-checked too
// (MakeDependencyContextItems' own passes_firewall parameter) — a
// dependency edge can no longer name a firewalled file by path.
//
// Backend Context Provider retrieval (docs/MASTER_SPEC.md #71): when
// `Options::backend_registry` is set, every registered Backend's own
// IBackend::ProvideContext(intent) is consulted too (native Backends
// that override it, or a Plugin Backend forwarding through AbiV1.h's
// optional provide_context) — the fifth signal, alongside Symbol/
// Dependency/File/Keyword above, deduplicated by id the same way. Each
// returned item's `id` is passed through the same firewall check the
// other four sources use, treating it as a possible file path even
// though a Backend-provided id isn't guaranteed to look like one (e.g.
// a Git commit hash) — an id that doesn't resemble a path just never
// matches a deny pattern and passes through, but this still catches the
// case that matters (a Backend echoing back an actual firewalled path).
//
// Active File Bias (docs/ROADMAP.md "## IDE" -- the
// "ContextRetriever/context_retrieveのアクティブファイルへのバイアス付け"
// follow-up CLAUDE.md left explicitly out of scope when EditorStateStore
// itself was built): when `Options::editor_state_store` is set and it
// reports an active document, every Symbol/Dependency/File/Keyword item
// (the four sources with a real single-file identity — Backend Context
// Provider items are a different kind of relevance and are deliberately
// left unbiased, matching this class's own existing per-source-kind
// firewall treatment) whose underlying file is that active document, or
// a file `Options::include_graph` reports as directly included by/
// including it, gets its already-computed priority pushed toward the
// docs/MASTER_SPEC.md #14 "100 = 現在の編集対象" ceiling — a boost applied
// after each source's own scoring, never a replacement for it, so a
// symbol/file/keyword match unrelated to the active file still surfaces
// exactly as before. nullptr (the default) applies no bias at all,
// unchanged from before this was added — the same opt-in, nullable-
// pointer convention `Options::firewall`/`Options::backend_registry`
// already use here, and `McpServerOptions::editor_state_store`/
// `context_cache` use one level up.
class ContextRetriever {
public:
    // Which of Symbol/File/Keyword retrieval hit its Options::max_*_matches
    // cap and had to drop lower-ranked candidates to fit (found via manual
    // testing, 2026-09-18: a broad intent silently came back with exactly
    // max_symbol_matches items and no signal that more relevant symbols
    // existed but were cut — unlike ContextSelector's budget-driven
    // exclusions, which are visible in ContextSelection::excluded, this
    // truncation previously had no caller-visible trace at all). Dependency
    // and Backend Context Provider retrieval have no cap of their own, so
    // they have no corresponding field here. Known imprecision: each
    // source's own per-token search call (see Retrieve()) is itself capped
    // at max_*_matches before the tokens' results are merged, so a single
    // dominant token could already have discarded matches that a flag here
    // won't report if the final merged count happens to land at or under
    // the cap — this only guarantees "the final list was cut", not "no
    // source-level cut ever happened".
    struct RetrievalTruncation {
        bool symbol = false;
        bool file = false;
        bool keyword = false;

        [[nodiscard]] bool Any() const { return symbol || file || keyword; }
    };

    struct Options {
        const SymbolIndex* symbol_index = nullptr;
        const IncludeGraph* include_graph = nullptr;
        // Root to scan for File retrieval (path matching) and Keyword
        // retrieval; empty skips both (Symbol retrieval still runs if
        // symbol_index is set).
        std::string project_root;
        // nullptr (default) applies no firewall, unchanged from before
        // this was added.
        const Sandbox* firewall = nullptr;
        // nullptr (default) skips Backend Context Provider retrieval
        // entirely, unchanged from before this was added.
        const BackendRegistry* backend_registry = nullptr;
        // nullptr (default) applies no Active File Bias at all, unchanged
        // from before this was added -- see class comment. When set,
        // Retrieve() calls Read() itself (no caching, same as every other
        // consumer of this class -- see EditorStateStore's own comment on
        // why) so it always sees the IDE's current active document, not
        // whatever it was when this Options struct was built. A state
        // file that's missing, stale-by-schema, or reports no active
        // document (EditorStateStore::Read() returning nullopt, or a
        // value whose active_document_path is itself nullopt) is treated
        // as "no bias to apply" -- the same graceful, error-free
        // degradation EditorStateStore::Read() itself documents, not a
        // failure this class surfaces. Wall-clock staleness of the
        // report (an IDE session that closed a while ago, leaving a
        // last-known-state file behind) is a known, undecided gap: the
        // one field that could answer it, EditorState::updated_at, is
        // still the raw, unparsed ISO-8601 string EditorState.hpp's own
        // comment already flags as "kept for a future staleness check,
        // which this slice deliberately doesn't implement yet" -- adding
        // a wall-clock TTL comparison here would be inventing exactly the
        // naive-TTL mechanism AGENT.md #7 asks Cache-like features to
        // avoid, over a field no other part of this codebase has decided
        // a real (hash/mtime/version-style) staleness policy for yet.
        // Left unimplemented rather than guessed at (AGENT.md #14).
        const EditorStateStore* editor_state_store = nullptr;
        std::size_t max_symbol_matches = 5;
        std::size_t max_file_matches = 5;
        std::size_t max_keyword_matches = 10;
        // When true, the FileScanner::Scan() File/Keyword retrieval share
        // (every file's metadata + content, read and hashed from disk) is
        // done once and reused by every later Retrieve() call until
        // InvalidateScan() is called. Profiling this project's own tree
        // (2026-09-18, ~340 files, Debug build) put that rescan at
        // ~750ms of a ~1.7s warm context_retrieve -- the single largest
        // phase, paid on every call even though RunMcpMode() already runs
        // a FileWatcher that knows exactly when a file changed. The
        // caller that opts in owns wiring InvalidateScan() to that
        // "FileChanged" event (see Core/src/main.cpp), the same
        // bootstrap-site subscription shape ContextCache::InvalidateAll()
        // uses; without such a trigger a cached scan is exactly the
        // never-invalidated cache AGENT.md #7 warns against, so this stays
        // opt-in. false (default) rescans on every call, unchanged from
        // before this existed -- which is also why every unit test that
        // writes files between two Retrieve() calls keeps working as-is.
        bool cache_scan = false;
    };

    explicit ContextRetriever(Options options) : options_(std::move(options)) {}

    // Empty intent returns an empty result rather than matching
    // everything. `truncation`, when non-null, is written with which
    // sources hit their Options::max_*_matches cap — nullptr (the default)
    // skips that entirely, unchanged from before this parameter existed.
    [[nodiscard]] std::vector<ContextItem> Retrieve(const std::string& intent,
                                                       RetrievalTruncation* truncation = nullptr) const;

    // Same Read() Retrieve() itself does for Active File Bias -- exposed
    // so ContextCache can fold the active document into its cache key
    // without duplicating EditorStateStore access logic (docs/ROADMAP.md
    // CE-4). nullopt under the exact same conditions Retrieve()'s bias is
    // a no-op (no editor_state_store, no state file, no active document).
    [[nodiscard]] std::optional<std::string> CurrentActiveDocumentPath() const;

    // Discards the scan Options::cache_scan kept, so the next Retrieve()
    // reads the project from disk again. No-op when cache_scan is false.
    // Safe to call from a thread other than Retrieve()'s (FileWatcher
    // publishes "FileChanged" from its own): a Retrieve() already past
    // ScanProject() keeps using the snapshot it took, and one that
    // started scanning before this call never publishes its stale result
    // (see ScanProject()).
    void InvalidateScan();

private:
    // One FileScanner::Scan() pass: `files` for File retrieval's path
    // matching, `contents` (filled by that same pass) for Keyword
    // retrieval's line matching. shared_ptr so a Retrieve() call holds
    // its own reference for the duration and InvalidateScan() racing
    // with it just drops the cache's reference, never the data underfoot.
    struct ScanSnapshot {
        std::vector<FileMetadata> files;
        FileCache contents;
    };

    // Cached snapshot when Options::cache_scan is on and one is valid;
    // otherwise a fresh scan. nullptr if the scan itself failed.
    [[nodiscard]] std::shared_ptr<ScanSnapshot> ScanProject() const;

    Options options_;
    mutable std::mutex scan_mutex_;
    mutable std::shared_ptr<ScanSnapshot> scan_snapshot_;
    // Bumped by InvalidateScan(); a scan that began under an older
    // generation is not cached (it may predate the change that
    // invalidated it), only returned to its own caller.
    mutable std::uint64_t scan_generation_ = 0;
};

} // namespace aistudio::core
