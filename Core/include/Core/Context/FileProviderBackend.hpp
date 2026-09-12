#pragma once

#include "Core/Backend/IBackend.hpp"
#include "Core/Cache/Cache.hpp"
#include "Core/Project/FileCache.hpp"
#include "Core/Project/FileMetadata.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace aistudio::core {

// Context Provider SDK's first concrete Provider (docs/MASTER_SPEC.md #71
// "FileProvider", docs/ROADMAP.md "Context Provider SDK"). Wraps
// FileScanner + MakeFileContextItem's content overload as an IBackend so
// any ContextRetriever constructed with a BackendRegistry -- not just one
// given an explicit project_root -- can pull File-sourced context via
// IBackend::ProvideContext().
//
// Deliberately scores by file *content* occurrences of the intent, not by
// filename/path (which is what ContextRetriever's own built-in "File
// retrieval" step already does with metadata-only items). Retrieve()
// drops a later item whose id was already added by an earlier stage
// (ContextRetriever.cpp's `add_item`), and Backend Context Provider
// retrieval runs last -- if this class re-scored by filename too, its
// content-bearing items for files the built-in step already claimed would
// just be silently discarded. Scoring by content instead means this
// Backend mostly surfaces files the built-in step wouldn't have picked,
// making its contribution additive rather than redundant.
class FileProviderBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "core.file_provider"; }
    [[nodiscard]] std::string Name() const override { return "File Context Provider"; }
    [[nodiscard]] std::string Version() const override { return "0.1.0"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override {
        return {"context.provide.file@1.0.0"};
    }

    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }

    // Subscribes to "FileChanged" to invalidate scan_cache_ below.
    Result<void> Start() override;
    Result<void> Stop() override;

    // Reads `backend.core.file_provider.root` (falling back to
    // `project.root`, then ".") and optionally
    // `backend.core.file_provider.max_results` /
    // `backend.core.file_provider.context_lines`.
    Result<void> Configure(const Config& config) override;

    // No commands/queries yet -- this Backend only contributes context.
    CommandResult Dispatch(const Command& command) override;
    QueryResult Handle(const Query& query) override;

    [[nodiscard]] std::vector<ContextItem> ProvideContext(const std::string& intent) const override;

private:
    std::string root_;
    std::size_t max_results_ = 3;
    // Lines kept on each side of a matching line; windows are merged, and
    // a file whose windows cover it comes back whole. Results used to
    // carry entire files (up to 256 KiB each), so the max_results_ cap
    // never bounded how much text a client actually received.
    std::size_t context_lines_ = 20;
    mutable FileCache cache_; // ProvideContext() is const; content reuse across calls needs mutable

    // Caches FileScanner::Scan(root_) itself, keyed "scan"; Cache<T>'s own
    // mutex covers the cross-thread invalidation from "FileChanged".
    mutable Cache<std::vector<FileMetadata>> scan_cache_;
    int file_changed_subscription_id_ = 0;
};

} // namespace aistudio::core
