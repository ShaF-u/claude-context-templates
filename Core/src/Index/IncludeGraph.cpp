#include "Core/Index/IncludeGraph.hpp"

#include "Core/Index/IncludeExtractor.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>

namespace aistudio::core {

namespace {
constexpr std::array<const char*, 6> kSourceExtensions = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx"};

// True if `path` (a FileScanner-produced, '/'-separated project-relative
// path) ends with `suffix` on a path-component boundary — i.e. `suffix`
// wasn't matched against the middle of some other component's name.
bool EndsWithPathSegment(const std::string& path, const std::string& suffix) {
    if (suffix.empty() || suffix.size() > path.size()) {
        return false;
    }
    const auto offset = path.size() - suffix.size();
    if (path.compare(offset, suffix.size(), suffix) != 0) {
        return false;
    }
    return offset == 0 || path[offset - 1] == '/';
}

// Resolves an include's raw text (e.g. "Core/Index/Symbol.hpp") to a
// scanned project file, preferring the shortest matching path — see
// IncludeGraph.hpp's class comment for why suffix matching is used
// instead of modeling the compiler's actual include search paths.
std::optional<std::string> ResolveInclude(const std::string& include_text, const std::vector<std::string>& all_paths) {
    const std::string* best = nullptr;
    for (const auto& candidate : all_paths) {
        if (!EndsWithPathSegment(candidate, include_text)) {
            continue;
        }
        if (best == nullptr || candidate.size() < best->size() ||
            (candidate.size() == best->size() && candidate < *best)) {
            best = &candidate;
        }
    }
    return best != nullptr ? std::optional(*best) : std::nullopt;
}

} // namespace

bool IncludeGraph::IsSourceFile(const std::string& path) {
    const auto extension = Utf8ToPath(path).extension();
    return std::any_of(kSourceExtensions.begin(), kSourceExtensions.end(),
                        [&](const char* candidate) { return extension == candidate; });
}

Result<void> IncludeGraph::Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns) {
    const FileScanner scanner(FileScanner::MakeOptions(extra_ignore_patterns));
    const auto scan_result = scanner.Scan(root);
    if (!scan_result) {
        return Result<void>::Fail(scan_result.Err());
    }

    std::vector<std::string> all_paths;
    all_paths.reserve(scan_result.Value().size());
    for (const auto& metadata : scan_result.Value()) {
        all_paths.push_back(metadata.path);
    }

    std::vector<IncludeEdge> edges;
    const IncludeExtractor extractor;
    const std::filesystem::path root_path = Utf8ToPath(root);

    for (const auto& metadata : scan_result.Value()) {
        if (!IsSourceFile(metadata.path)) {
            continue;
        }

        std::vector<IncludeEdge> file_edges;
        if (auto cached = cache_.Get(metadata.path, metadata.content_hash)) {
            file_edges = std::move(*cached);
        } else {
            std::ifstream file(root_path / Utf8ToPath(metadata.path), std::ios::binary);
            if (!file.is_open()) {
                continue;
            }
            std::ostringstream buffer;
            buffer << file.rdbuf();

            file_edges = extractor.Extract(metadata.path, buffer.str());
            Cache<std::vector<IncludeEdge>>::PutOptions options;
            options.version = metadata.content_hash;
            cache_.Put(metadata.path, file_edges, options);
        }

        for (auto& edge : file_edges) {
            if (!edge.is_system) {
                if (const auto resolved = ResolveInclude(edge.include_text, all_paths); resolved.has_value()) {
                    edge.resolved_path = *resolved;
                }
            }
            edges.push_back(std::move(edge));
        }
    }

    {
        std::lock_guard lock(mutex_);
        edges_ = std::move(edges);
        known_paths_ = std::move(all_paths);
    }
    return Result<void>::Ok();
}

void IncludeGraph::ReResolveAllEdgesLocked() {
    for (auto& edge : edges_) {
        edge.resolved_path.clear();
        if (!edge.is_system) {
            if (const auto resolved = ResolveInclude(edge.include_text, known_paths_); resolved.has_value()) {
                edge.resolved_path = *resolved;
            }
        }
    }
}

Result<void> IncludeGraph::UpdateFile(const std::string& root, const std::string& path) {
    std::vector<IncludeEdge> file_edges;
    const bool is_source = IsSourceFile(path);
    if (is_source) {
        const std::filesystem::path root_path = Utf8ToPath(root);
        std::ifstream file(root_path / Utf8ToPath(path), std::ios::binary);
        if (!file.is_open()) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::IOError,
                .message = "failed to open file: " + path,
                .module = "Core.Index.IncludeGraph",
            });
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string content = buffer.str();

        const IncludeExtractor extractor;
        file_edges = extractor.Extract(path, content);

        Cache<std::vector<IncludeEdge>>::PutOptions options;
        options.version = HashContent(content);
        cache_.Put(path, file_edges, options);
    }

    std::lock_guard lock(mutex_);
    if (std::find(known_paths_.begin(), known_paths_.end(), path) == known_paths_.end()) {
        known_paths_.push_back(path);
    }
    if (is_source) {
        edges_.erase(
            std::remove_if(edges_.begin(), edges_.end(), [&](const IncludeEdge& e) { return e.from_file == path; }),
            edges_.end());
        edges_.insert(edges_.end(), std::make_move_iterator(file_edges.begin()),
                       std::make_move_iterator(file_edges.end()));
    }
    // A new/changed path can change what OTHER edges resolve to, not just
    // this file's own -- see the class comment.
    ReResolveAllEdgesLocked();
    return Result<void>::Ok();
}

Result<void> IncludeGraph::RemoveFile(const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        known_paths_.erase(std::remove(known_paths_.begin(), known_paths_.end(), path), known_paths_.end());
        edges_.erase(
            std::remove_if(edges_.begin(), edges_.end(), [&](const IncludeEdge& e) { return e.from_file == path; }),
            edges_.end());
        ReResolveAllEdgesLocked();
    }
    cache_.Invalidate(path);
    return Result<void>::Ok();
}

std::vector<std::string> IncludeGraph::Includes(const std::string& file_path) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    for (const auto& edge : edges_) {
        if (edge.from_file == file_path && !edge.resolved_path.empty()) {
            result.push_back(edge.resolved_path);
        }
    }
    return result;
}

std::vector<std::string> IncludeGraph::IncludedBy(const std::string& file_path) const {
    std::lock_guard lock(mutex_);
    std::vector<std::string> result;
    for (const auto& edge : edges_) {
        if (edge.resolved_path == file_path) {
            result.push_back(edge.from_file);
        }
    }
    return result;
}

std::vector<IncludeEdge> IncludeGraph::AllEdges() const {
    std::lock_guard lock(mutex_);
    return edges_;
}

std::size_t IncludeGraph::Size() const {
    std::lock_guard lock(mutex_);
    return edges_.size();
}

} // namespace aistudio::core
