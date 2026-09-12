#pragma once

#include "Core/Context/ContextItem.hpp"
#include "Core/Index/IncludeGraph.hpp"

#include <functional>
#include <string>
#include <vector>

namespace aistudio::core {

// Turns a file's direct include relationships (IncludeGraph) into
// ContextItems — Context Retrieval's "Dependency retrieval"
// (docs/ROADMAP.md Phase 2), now that Phase 3's IncludeGraph exists to
// retrieve from. Each returned item names one relationship rather than
// embedding the related file's content (CompressionLevel::Reference —
// docs/ROADMAP.md Phase 2 "Context Compression" > "Reference
// representation"), so pulling in a file's dependency list stays cheap
// even when it has many includes. A caller that decides a specific
// dependency actually matters can still retrieve its full content
// separately (e.g. via MakeFileContextItem) — this only tells the
// Context Engine the relationship exists.
//
// `direction` controls which edges are surfaced: Includes (what
// file_path drags in — relevant when editing file_path itself) or
// IncludedBy (what would be affected by changing file_path — the first
// building block toward Phase 3 "Impact Analysis"). Unresolved edges
// (system headers, anything outside the project) are never included,
// since a ContextItem needs an actual project file to eventually retrieve.
enum class DependencyDirection { Includes, IncludedBy };

// `passes_firewall`, when set, skips any related_path it rejects -- a
// Dependency edge otherwise names a firewalled file by path even though
// its content is never exposed (Context Firewall, docs/MASTER_SPEC.md
// #21). Default nullptr means "no filtering", matching every existing
// call site's pre-existing unrestricted behavior.
[[nodiscard]] std::vector<ContextItem> MakeDependencyContextItems(
    const IncludeGraph& graph, const std::string& file_path, DependencyDirection direction, int priority = 40,
    const std::function<bool(const std::string&)>& passes_firewall = nullptr);

} // namespace aistudio::core
