#include "Core/Index/SymbolIndex.hpp"

#include "Core/Index/SymbolExtractor.hpp"
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

bool SymbolIndex::IsSourceFile(const std::string& path) {
    const auto extension = Utf8ToPath(path).extension();
    return std::any_of(kSourceExtensions.begin(), kSourceExtensions.end(),
                        [&](const char* candidate) { return extension == candidate; });
}

Result<void> SymbolIndex::Build(const std::string& root, const std::vector<std::string>& extra_ignore_patterns) {
    const FileScanner scanner(FileScanner::MakeOptions(extra_ignore_patterns));
    const auto scan_result = scanner.Scan(root);
    if (!scan_result) {
        return Result<void>::Fail(scan_result.Err());
    }

    std::vector<Symbol> symbols;
    const SymbolExtractor extractor;
    const std::filesystem::path root_path = Utf8ToPath(root);

    for (const auto& metadata : scan_result.Value()) {
        if (!IsSourceFile(metadata.path)) {
            continue;
        }

        if (auto cached = cache_.Get(metadata.path, metadata.content_hash)) {
            symbols.insert(symbols.end(), std::make_move_iterator(cached->begin()),
                            std::make_move_iterator(cached->end()));
            continue;
        }

        std::ifstream file(root_path / Utf8ToPath(metadata.path), std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        auto file_symbols = extractor.Extract(metadata.path, buffer.str());
        Cache<std::vector<Symbol>>::PutOptions options;
        options.version = metadata.content_hash;
        cache_.Put(metadata.path, file_symbols, options);
        symbols.insert(symbols.end(), std::make_move_iterator(file_symbols.begin()),
                        std::make_move_iterator(file_symbols.end()));
    }

    {
        std::lock_guard lock(mutex_);
        symbols_ = std::move(symbols);
    }
    return Result<void>::Ok();
}

Result<void> SymbolIndex::UpdateFile(const std::string& root, const std::string& path) {
    if (!IsSourceFile(path)) {
        return Result<void>::Ok();
    }

    const std::filesystem::path root_path = Utf8ToPath(root);
    std::ifstream file(root_path / Utf8ToPath(path), std::ios::binary);
    if (!file.is_open()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to open file: " + path,
            .module = "Core.Index.SymbolIndex",
        });
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string content = buffer.str();

    const SymbolExtractor extractor;
    auto file_symbols = extractor.Extract(path, content);

    Cache<std::vector<Symbol>>::PutOptions options;
    options.version = HashContent(content);
    cache_.Put(path, file_symbols, options);

    {
        std::lock_guard lock(mutex_);
        symbols_.erase(
            std::remove_if(symbols_.begin(), symbols_.end(), [&](const Symbol& s) { return s.file_path == path; }),
            symbols_.end());
        symbols_.insert(symbols_.end(), std::make_move_iterator(file_symbols.begin()),
                         std::make_move_iterator(file_symbols.end()));
    }
    return Result<void>::Ok();
}

Result<void> SymbolIndex::RemoveFile(const std::string& path) {
    {
        std::lock_guard lock(mutex_);
        symbols_.erase(
            std::remove_if(symbols_.begin(), symbols_.end(), [&](const Symbol& s) { return s.file_path == path; }),
            symbols_.end());
    }
    cache_.Invalidate(path);
    return Result<void>::Ok();
}

std::vector<Symbol> SymbolIndex::FindByName(const std::string& name) const {
    std::lock_guard lock(mutex_);
    std::vector<Symbol> result;
    for (const auto& symbol : symbols_) {
        if (symbol.name == name) {
            result.push_back(symbol);
        }
    }
    return result;
}

std::vector<Symbol> SymbolIndex::All() const {
    std::lock_guard lock(mutex_);
    return symbols_;
}

std::size_t SymbolIndex::Size() const {
    std::lock_guard lock(mutex_);
    return symbols_.size();
}

} // namespace aistudio::core
