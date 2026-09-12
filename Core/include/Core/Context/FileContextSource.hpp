#pragma once

#include "Core/Context/ContextItem.hpp"
#include "Core/Project/FileMetadata.hpp"

#include <string>

namespace aistudio::core {

// Turns a scanned file into a ContextItem — the first hookup point for
// Context Retrieval's "File retrieval" (docs/ROADMAP.md Phase 2). Symbol/
// Dependency/Git retrieval follow the same shape once Project
// Intelligence (Phase 3) and Git Backend (Phase 6) exist.
//
// This overload estimates tokens from FileMetadata::size alone, so
// callers don't need to load a file's content just to decide whether it's
// a selection candidate.
[[nodiscard]] ContextItem MakeFileContextItem(const FileMetadata& metadata, int priority = 50);

// This overload attaches the actual content (e.g. loaded via FileCache)
// and estimates tokens from it directly, which is more accurate than the
// size-based estimate above.
[[nodiscard]] ContextItem MakeFileContextItem(const FileMetadata& metadata, std::string content, int priority = 50);

} // namespace aistudio::core
