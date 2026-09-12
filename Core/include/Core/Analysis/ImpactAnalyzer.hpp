#pragma once

#include "Core/Index/CallGraph.hpp"
#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// One function/method defined in the target file, and who — elsewhere in
// the project — calls it (CallGraph::Callers; see that class for why
// this is necessarily a heuristic, name-based match rather than true
// call resolution).
struct AffectedSymbol {
    std::string symbol_name;
    std::vector<CallEdge> callers;
};

// docs/ROADMAP.md Phase 3 "Impact Analysis" > "Dependency impact" /
// "Change scope estimation": everything IncludeGraph and CallGraph can
// currently say about what changing one file might affect.
struct ImpactResult {
    std::string target_file;
    // Every file that transitively includes target_file, directly or
    // through another file — IncludeGraph::IncludedBy() followed
    // recursively, deduplicated, target_file itself excluded.
    std::vector<std::string> affected_files;
    // Every function/method Symbol defined in target_file, together with
    // its (heuristically matched) call sites elsewhere.
    std::vector<AffectedSymbol> affected_symbols;
};

// One Symbol (any kind) whose line overlaps a git diff's changed lines —
// see AnalyzeChanges(). `callers` is only ever populated for
// SymbolKind::Function (mirrors AffectedSymbol/Analyze()'s own scope —
// CallGraph::Callers() only means something for functions).
struct ChangedSymbol {
    std::string symbol_name;
    SymbolKind kind;
    std::string file_path;
    std::vector<CallEdge> callers;
};

// docs/ROADMAP.md Phase 3 "Impact Analysis" > "Changed symbol detection"
// / Phase 6 "Git Intelligence" > "Changed symbol detection": the
// diff-based counterpart to ImpactResult (see AnalyzeChanges()).
struct ChangeImpactResult {
    // Every file the diff touches (from its "+++ b/<path>" headers),
    // regardless of whether any Symbol in it happened to overlap a
    // changed line.
    std::vector<std::string> changed_files;
    std::vector<ChangedSymbol> changed_symbols;
    // Union of CollectTransitiveIncluders() over every changed_files
    // entry — every file that transitively includes ANY changed file.
    std::vector<std::string> affected_files;
};

// Combines IncludeGraph + CallGraph + SymbolIndex to answer "if I change
// this file, what else might be affected?" — deliberately a thin
// combinator over three already-built graphs, not a fourth index of its
// own: no Build() step, no Cache<T>, no ownership of the inputs.
// Construct one whenever the three graphs are already built (e.g. right
// after aistudio_core_cli's bootstrap indexing) and ask it questions.
//
// What this does NOT do (docs/ROADMAP.md Phase 3 "Impact Analysis"
// still lists these as separate, unimplemented items): "Asset impact"
// needs an asset system that doesn't exist yet. "Risk estimation" needs
// a real signal (test coverage, change history) beyond raw edge counts,
// which would be a hollow number dressed up as a metric — callers can
// derive a coarse proxy from affected_files.size() /
// affected_symbols.size() themselves if that's good enough for now.
// "Changed symbol detection" is now implemented (AnalyzeChanges()) —
// SymbolIndex has no end-line for a Symbol, so its span is approximated
// as "up to the next Symbol in the same file, or end of file"; a
// genuinely deleted Symbol can't appear in changed_symbols since
// SymbolIndex only ever reflects the current working tree, not history
// (it'll still surface in changed_files).
class ImpactAnalyzer {
public:
    ImpactAnalyzer(const IncludeGraph& include_graph, const CallGraph& call_graph, const SymbolIndex& symbol_index);

    [[nodiscard]] ImpactResult Analyze(const std::string& file_path) const;

    // `unified_diff` is expected to be `git diff`'s raw stdout (e.g.
    // GitBackend's "git.diff" Query result) -- unstaged changes against
    // the working tree, the only shape GitBackend currently produces. A
    // deleted file's "+++ /dev/null" side contributes nothing (no lines
    // exist in a deleted file to overlap a Symbol's span with).
    [[nodiscard]] ChangeImpactResult AnalyzeChanges(const std::string& unified_diff) const;

private:
    [[nodiscard]] std::vector<std::string> CollectTransitiveIncluders(const std::vector<std::string>& seeds) const;

    const IncludeGraph& include_graph_;
    const CallGraph& call_graph_;
    const SymbolIndex& symbol_index_;
};

} // namespace aistudio::core
