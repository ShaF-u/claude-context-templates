#include "Core/Context/ContextGarbageCollector.hpp"

#include <algorithm>

namespace aistudio::core {

Result<std::size_t> ContextGarbageCollector::CollectGarbage() const {
    const auto all_result = repository_.FindAll();
    if (!all_result) {
        return Result<std::size_t>::Fail(all_result.Err());
    }
    auto snapshots = all_result.Value();
    if (snapshots.size() <= options_.max_snapshots_to_keep) {
        return Result<std::size_t>::Ok(0);
    }

    // Newest first; FindAll() already returns insertion order (ORDER BY
    // rowid), so a stable sort on created_at preserves that ordering as
    // the tie-breaker between equal timestamps.
    std::stable_sort(snapshots.begin(), snapshots.end(), [](const ContextSnapshot& a, const ContextSnapshot& b) {
        return a.created_at > b.created_at;
    });

    std::size_t removed = 0;
    for (std::size_t i = options_.max_snapshots_to_keep; i < snapshots.size(); ++i) {
        if (const auto remove_result = repository_.Remove(snapshots[i].id); !remove_result) {
            return Result<std::size_t>::Fail(remove_result.Err());
        }
        ++removed;
    }
    return Result<std::size_t>::Ok(removed);
}

} // namespace aistudio::core
