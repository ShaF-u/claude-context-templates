#include "Core/Index/AstIndex.hpp"

#include "Core/Index/AstExtractor.hpp"
#include "Core/Project/FileScanner.hpp"
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

bool AstIndex::IsSourceFile(const std::string& path) {
    const auto extension = Utf8ToPath(path).extension();
    return std::any_of(kSourceExtensions.begin(), kSourceExtensions.end(),
                        [&](const char* candidate) { return extension == candidate; });
}

Result<void> AstIndex::Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns) {
    const FileScanner scanner(FileScanner::MakeOptions(extra_ignore_patterns));
    const auto scan_result = scanner.Scan(root);
    if (!scan_result) {
        return Result<void>::Fail(scan_result.Err());
    }

    std::unordered_map<std::string, AstNode> trees;
    const AstExtractor extractor;
    const std::filesystem::path root_path = Utf8ToPath(root);

    for (const auto& metadata : scan_result.Value()) {
        if (!IsSourceFile(metadata.path)) {
            continue;
        }

        if (auto cached = cache_.Get(metadata.path, metadata.content_hash)) {
            trees.emplace(metadata.path, std::move(*cached));
            continue;
        }

        std::ifstream file(root_path / Utf8ToPath(metadata.path), std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        auto tree = extractor.Extract(buffer.str());
        Cache<AstNode>::PutOptions options;
        options.version = metadata.content_hash;
        cache_.Put(metadata.path, tree, options);
        trees.emplace(metadata.path, std::move(tree));
    }

    {
        std::lock_guard lock(mutex_);
        trees_ = std::move(trees);
    }
    return Result<void>::Ok();
}

Result<void> AstIndex::UpdateFile(const std::string& root, const std::string& path) {
    if (!IsSourceFile(path)) {
        return Result<void>::Ok();
    }

    const std::filesystem::path root_path = Utf8ToPath(root);
    std::ifstream file(root_path / Utf8ToPath(path), std::ios::binary);
    if (!file.is_open()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to open file: " + path,
            .module = "Core.Index.AstIndex",
        });
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    const AstExtractor extractor;
    auto tree = extractor.Extract(content);

    Cache<AstNode>::PutOptions options;
    options.version = HashContent(content);
    cache_.Put(path, tree, options);

    std::lock_guard lock(mutex_);
    trees_.insert_or_assign(path, std::move(tree));
    return Result<void>::Ok();
}

Result<void> AstIndex::RemoveFile(const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        trees_.erase(path);
    }
    cache_.Invalidate(path);
    return Result<void>::Ok();
}

std::optional<AstNode> AstIndex::Get(const std::string& file_path) const {
    std::lock_guard lock(mutex_);
    const auto it = trees_.find(file_path);
    if (it == trees_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::size_t AstIndex::Size() const {
    std::lock_guard lock(mutex_);
    return trees_.size();
}

} // namespace aistudio::core
