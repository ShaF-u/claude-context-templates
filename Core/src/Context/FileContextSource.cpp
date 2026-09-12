#include "Core/Context/FileContextSource.hpp"

namespace aistudio::core {

ContextItem MakeFileContextItem(const FileMetadata& metadata, int priority) {
    ContextItem item;
    item.id = metadata.path;
    item.source = ContextSourceKind::File;
    item.priority = priority;
    item.estimated_tokens = static_cast<std::int64_t>((metadata.size + 3) / 4);
    return item;
}

ContextItem MakeFileContextItem(const FileMetadata& metadata, std::string content, int priority) {
    ContextItem item;
    item.id = metadata.path;
    item.source = ContextSourceKind::File;
    item.priority = priority;
    item.estimated_tokens = EstimateTokens(content);
    item.content = std::move(content);
    return item;
}

} // namespace aistudio::core
