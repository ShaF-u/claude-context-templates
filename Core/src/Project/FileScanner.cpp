#include "Core/Project/FileScanner.hpp"

#include "Core/Util/Glob.hpp"
#include "Core/Util/Utf8.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace aistudio::core {

namespace {

namespace fs = std::filesystem;

std::vector<std::string> SplitSegments(const std::string& relative_path) {
    std::vector<std::string> segments;
    std::string current;
    for (const char c : relative_path) {
        if (c == '/' || c == '\\') {
            if (!current.empty()) {
                segments.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        segments.push_back(current);
    }
    return segments;
}

} // namespace

std::vector<std::string> FileScanner::DefaultIgnorePatterns() {
    return {
        ".git",         "node_modules",  "build", "dist",   ".vs",
        ".vscode",      ".ts-build",     "__pycache__",
        // MSVC per-project intermediate/output directories -- without
        // these, FileScanner recurses into every project's Debug/Release
        // intermediate tree and reads+hashes whatever isn't caught by the
        // extension patterns below
        // (.tlog/.idb/.ilk/.ipch files, some tens of MB each), which is
        // what made a full-repo Scan()/Index::Build() take long enough to
        // blow past an MCP client's connection timeout in a solution with
        // several C++ projects.
        "x64",          "Win32",         "packages",
        // Vendored third-party source (e.g. Engine/external's bundled EnTT/
        // ImGui/SpdLog): not this project's own code, and re-tree-sitter-
        // parsing a multi-MB single-header library like entt.hpp once per
        // index (SymbolIndex/IncludeGraph/CallGraph/InheritanceGraph/
        // ReferenceGraph/AstIndex all call FileScanner independently) was
        // the other big contributor to Index::Build() blowing past an MCP
        // client's connection timeout.
        "external",     "vendor",        "third_party",
        "*.obj",        "*.pdb",         "*.exe", "*.lib",  "*.exp",
        "*.db",         "*.sqlite3",     "*.tsbuildinfo",
        "*.tlog",       "*.idb",         "*.ilk", "*.ipch", "*.iobj",
        "*.ipdb",       "*.res",         "*.log",
        // Binary game assets (App/Assets, Engine/Assets): large and not
        // meaningfully parseable/searchable as text, so scanning/hashing
        // them full-content only adds cost with no benefit to the tools
        // this indexes for (symbol_search/context_retrieve/etc.).
        "*.fbx",        "*.mdl",         "*.dds", "*.dll",
    };
}

FileScanner::Options FileScanner::MakeOptions(const std::vector<std::string>& extra_ignore_patterns) {
    Options options;
    options.ignore_patterns.insert(options.ignore_patterns.end(), extra_ignore_patterns.begin(),
                                    extra_ignore_patterns.end());
    return options;
}

bool FileScanner::IsIgnored(const std::string& relative_path) const {
    const std::string normalized = [&] {
        std::string p = relative_path;
        for (char& c : p) {
            if (c == '\\') c = '/';
        }
        return p;
    }();

    for (const auto& pattern : options_.ignore_patterns) {
        if (pattern.find('/') != std::string::npos) {
            if (GlobMatch(normalized, pattern)) {
                return true;
            }
            continue;
        }
        for (const auto& segment : SplitSegments(normalized)) {
            if (GlobMatch(segment, pattern)) {
                return true;
            }
        }
    }
    return false;
}

Result<std::vector<FileMetadata>> FileScanner::Scan(const std::string& root, FileCache* content_cache) const {
    std::error_code ec;
    const fs::path root_path = Utf8ToPath(root);
    if (!fs::exists(root_path, ec) || !fs::is_directory(root_path, ec)) {
        return Result<std::vector<FileMetadata>>::Fail(Error{
            .code = ErrorCode::NotFound,
            .message = "not a directory: " + root,
            .module = "Core.Project.FileScanner",
        });
    }

    std::vector<FileMetadata> results;
    fs::recursive_directory_iterator it(root_path, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    for (; !ec && it != end; it.increment(ec)) {
        const fs::directory_entry entry = *it;
        std::error_code relative_ec;
        const fs::path relative = fs::relative(entry.path(), root_path, relative_ec);
        if (relative_ec) {
            continue;
        }
        const std::string relative_str = PathToUtf8Generic(relative);

        std::error_code is_dir_ec;
        const bool is_directory = entry.is_directory(is_dir_ec);

        if (IsIgnored(relative_str)) {
            if (!is_dir_ec && is_directory) {
                it.disable_recursion_pending();
            }
            continue;
        }

        std::error_code is_file_ec;
        if (is_dir_ec || is_file_ec || !entry.is_regular_file(is_file_ec)) {
            continue;
        }

        std::ifstream file(entry.path(), std::ios::binary);
        if (!file.is_open()) {
            continue;
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();
        const std::string content = buffer.str();

        FileMetadata metadata;
        metadata.path = relative_str;
        metadata.size = static_cast<std::uint64_t>(content.size());
        std::error_code time_ec;
        const auto write_time = fs::last_write_time(entry.path(), time_ec);
        metadata.last_write_time = time_ec ? 0 : write_time.time_since_epoch().count();
        metadata.content_hash = HashContent(content);

        if (content_cache != nullptr) {
            content_cache->Put(metadata, std::move(content));
        }

        results.push_back(std::move(metadata));
    }

    return Result<std::vector<FileMetadata>>::Ok(std::move(results));
}

std::string HashContent(const std::string& content) {
    std::uint64_t hash = 14695981039346656037ULL; // FNV-1a 64-bit offset basis
    constexpr std::uint64_t prime = 1099511628211ULL;
    for (const unsigned char c : content) {
        hash ^= c;
        hash *= prime;
    }
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << hash;
    return oss.str();
}

} // namespace aistudio::core
