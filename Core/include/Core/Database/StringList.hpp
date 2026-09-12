#pragma once

#include <string>
#include <vector>

namespace aistudio::core {

// Serializes a string list into one TEXT column (comma-joined) and back
// — shared by any Repository storing a `vector<string>` field
// (TaskRepository::depends_on, ContextSnapshotRepository::included_item_ids).
// Ids/paths in this codebase don't contain commas, so no escaping is
// implemented; revisit if that assumption ever changes.
[[nodiscard]] std::string JoinStringList(const std::vector<std::string>& items);
[[nodiscard]] std::vector<std::string> SplitStringList(const std::string& joined);

} // namespace aistudio::core
