#include "Core/Index/CallGraph.hpp"

#include "Core/Index/CallExtractor.hpp"
#include "Core/Project/FileScanner.hpp"
#include "Core/Util/Identifier.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aistudio::core {

namespace {
constexpr std::array<const char*, 6> kSourceExtensions = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hxx"};
} // namespace

bool CallGraph::IsSourceFile(const std::string& path) {
    const auto extension = Utf8ToPath(path).extension();
    return std::any_of(kSourceExtensions.begin(), kSourceExtensions.end(),
                        [&](const char* candidate) { return extension == candidate; });
}

Result<void> CallGraph::Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns) {
    const FileScanner scanner(FileScanner::MakeOptions(extra_ignore_patterns));
    const auto scan_result = scanner.Scan(root);
    if (!scan_result) {
        return Result<void>::Fail(scan_result.Err());
    }

    std::vector<CallEdge> edges;
    const CallExtractor extractor;
    const std::filesystem::path root_path = Utf8ToPath(root);

    for (const auto& metadata : scan_result.Value()) {
        if (!IsSourceFile(metadata.path)) {
            continue;
        }

        if (auto cached = cache_.Get(metadata.path, metadata.content_hash)) {
            edges.insert(edges.end(), std::make_move_iterator(cached->begin()), std::make_move_iterator(cached->end()));
            continue;
        }

        std::ifstream file(root_path / Utf8ToPath(metadata.path), std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        auto file_edges = extractor.Extract(metadata.path, buffer.str());
        Cache<std::vector<CallEdge>>::PutOptions options;
        options.version = metadata.content_hash;
        cache_.Put(metadata.path, file_edges, options);
        edges.insert(edges.end(), std::make_move_iterator(file_edges.begin()), std::make_move_iterator(file_edges.end()));
    }

    {
        std::lock_guard lock(mutex_);
        edges_ = std::move(edges);
    }
    return Result<void>::Ok();
}

Result<void> CallGraph::UpdateFile(const std::string& root, const std::string& path) {
    if (!IsSourceFile(path)) {
        return Result<void>::Ok();
    }

    const std::filesystem::path root_path = Utf8ToPath(root);
    std::ifstream file(root_path / Utf8ToPath(path), std::ios::binary);
    if (!file.is_open()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to open file: " + path,
            .module = "Core.Index.CallGraph",
        });
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    const CallExtractor extractor;
    auto file_edges = extractor.Extract(path, content);

    Cache<std::vector<CallEdge>>::PutOptions options;
    options.version = HashContent(content);
    cache_.Put(path, file_edges, options);

    std::lock_guard lock(mutex_);
    edges_.erase(std::remove_if(edges_.begin(), edges_.end(), [&](const CallEdge& e) { return e.caller_file == path; }),
                 edges_.end());
    edges_.insert(edges_.end(), std::make_move_iterator(file_edges.begin()), std::make_move_iterator(file_edges.end()));
    return Result<void>::Ok();
}

Result<void> CallGraph::RemoveFile(const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        edges_.erase(
            std::remove_if(edges_.begin(), edges_.end(), [&](const CallEdge& e) { return e.caller_file == path; }),
            edges_.end());
    }
    cache_.Invalidate(path);
    return Result<void>::Ok();
}

std::vector<CallEdge> CallGraph::Callees(const std::string& caller_name) const {
    std::lock_guard lock(mutex_);
    std::vector<CallEdge> result;
    for (const auto& edge : edges_) {
        if (edge.caller_name == caller_name) {
            result.push_back(edge);
        }
    }
    return result;
}

std::vector<CallEdge> CallGraph::Callers(const std::string& callee_name) const {
    std::lock_guard lock(mutex_);
    std::vector<CallEdge> result;
    for (const auto& edge : edges_) {
        if (edge.callee_text == callee_name || TrailingIdentifier(edge.callee_text) == callee_name) {
            result.push_back(edge);
        }
    }
    return result;
}

std::vector<CallEdge> CallGraph::AllEdges() const {
    std::lock_guard lock(mutex_);
    return edges_;
}

std::size_t CallGraph::Size() const {
    std::lock_guard lock(mutex_);
    return edges_.size();
}

} // namespace aistudio::core
