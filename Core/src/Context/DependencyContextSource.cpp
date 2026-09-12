#include "Core/Context/DependencyContextSource.hpp"

namespace aistudio::core {

namespace {

ContextItem MakeItem(const std::string& file_path, const std::string& related_path, DependencyDirection direction,
                      int priority) {
    ContextItem item;
    if (direction == DependencyDirection::Includes) {
        item.id = file_path + "->" + related_path;
        item.content = file_path + " includes " + related_path;
    } else {
        item.id = related_path + "->" + file_path;
        item.content = related_path + " includes " + file_path;
    }
    item.source = ContextSourceKind::Dependency;
    item.compression = CompressionLevel::Reference;
    item.priority = priority;
    item.estimated_tokens = EstimateTokens(item.content);
    item.depends_on = {file_path};
    return item;
}

} // namespace

std::vector<ContextItem> MakeDependencyContextItems(const IncludeGraph& graph, const std::string& file_path,
                                                      DependencyDirection direction, int priority,
                                                      const std::function<bool(const std::string&)>& passes_firewall) {
    const auto related = direction == DependencyDirection::Includes ? graph.Includes(file_path)
                                                                      : graph.IncludedBy(file_path);

    std::vector<ContextItem> items;
    items.reserve(related.size());
    for (const auto& related_path : related) {
        if (passes_firewall && !passes_firewall(related_path)) {
            continue;
        }
        items.push_back(MakeItem(file_path, related_path, direction, priority));
    }
    return items;
}

} // namespace aistudio::core
