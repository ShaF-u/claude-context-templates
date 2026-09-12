#include "Core/Context/ContextSnapshot.hpp"

#include "Core/Util/Time.hpp"

namespace aistudio::core {

ContextSnapshot MakeContextSnapshot(std::string id, std::string task_name, const ContextSelection& selection,
                                     std::int64_t max_tokens) {
    ContextSnapshot snapshot;
    snapshot.id = std::move(id);
    snapshot.task_name = std::move(task_name);
    snapshot.used_tokens = selection.used_tokens;
    snapshot.max_tokens = max_tokens;
    snapshot.included_item_ids.reserve(selection.included.size());
    snapshot.included_item_source_kinds.reserve(selection.included.size());
    for (const auto& item : selection.included) {
        snapshot.included_item_ids.push_back(item.id);
        snapshot.included_item_source_kinds.push_back(item.source);
    }
    snapshot.created_at = CurrentUnixTimestamp();
    return snapshot;
}

} // namespace aistudio::core
