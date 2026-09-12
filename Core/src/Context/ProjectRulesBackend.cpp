#include "Core/Context/ProjectRulesBackend.hpp"

#include "Core/Backend/BackendFactoryRegistry.hpp"
#include "Core/Util/Utf8.hpp"

#include <any>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace aistudio::core {

namespace {

namespace fs = std::filesystem;

std::vector<std::string> SplitCommaSeparated(const std::string& text) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string part = text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (!part.empty()) {
            parts.push_back(part);
        }
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return parts;
}

// Combined cap across ALL configured files (not per-file like
// FileProviderBackend's kMaxFileSizeBytes) -- rules content is meant to
// be a short, standing set of conventions, not a dumping ground; a
// runaway file shouldn't crowd out the rest of the context budget
// (AGENT.md #8 "Context First").
constexpr std::size_t kMaxCombinedChars = 65536; // 64 KiB

std::string ReadCombinedRules(const std::string& root, const std::vector<std::string>& files) {
    std::string combined;
    const fs::path root_path = Utf8ToPath(root);

    for (const auto& relative_path : files) {
        std::ifstream file(root_path / Utf8ToPath(relative_path), std::ios::binary);
        if (!file.is_open()) {
            continue; // most projects won't have every configured file -- expected, not an error
        }
        std::ostringstream buffer;
        buffer << file.rdbuf();

        if (!combined.empty()) {
            combined += "\n\n";
        }
        combined += "=== " + relative_path + " ===\n" + buffer.str();

        if (combined.size() > kMaxCombinedChars) {
            // Utf8SafeTruncationLength, not a raw resize(kMaxCombinedChars):
            // this project's own CLAUDE.md/AGENT.md are full of Japanese
            // text, and a naive byte-count cut can land mid-character,
            // producing invalid UTF-8 that crashes JSON serialization
            // downstream (docs/ROADMAP.md CE-5).
            combined.resize(Utf8SafeTruncationLength(combined, kMaxCombinedChars));
            combined += "\n... [truncated]";
            break;
        }
    }
    return combined;
}

} // namespace

Result<void> ProjectRulesBackend::Configure(const Config& config) {
    root_ = config.GetOr("backend.core.project_rules.root", config.GetOr("project.root", "."));
    files_ = SplitCommaSeparated(config.GetOr("backend.core.project_rules.files", "CLAUDE.md,AGENT.md"));
    return Result<void>::Ok();
}

CommandResult ProjectRulesBackend::Dispatch(const Command& command) {
    return CommandResult::Fail(Error{
        .code = ErrorCode::NotFound,
        .message = "core.project_rules does not support command: " + command.name + " (read-only)",
        .module = "Core.Context.ProjectRulesBackend",
    });
}

QueryResult ProjectRulesBackend::Handle(const Query& query) {
    if (query.name == "rules.get") {
        return QueryResult::Ok(std::any(ReadCombinedRules(root_, files_)));
    }
    return QueryResult::Fail(Error{
        .code = ErrorCode::NotFound,
        .message = "core.project_rules does not support query: " + query.name,
        .module = "Core.Context.ProjectRulesBackend",
    });
}

std::vector<ContextItem> ProjectRulesBackend::ProvideContext(const std::string& intent) const {
    std::vector<ContextItem> items;
    if (root_.empty() || intent.empty()) {
        return items;
    }

    const std::string combined = ReadCombinedRules(root_, files_);
    if (combined.empty()) {
        return items;
    }

    ContextItem item;
    item.id = "rules/project";
    item.source = ContextSourceKind::Custom;
    item.compression = CompressionLevel::Raw;
    item.content = combined;
    // High and constant -- unlike Symbol/File/Git matches (whose
    // priority reflects how well they matched THIS intent), project
    // rules are supposed to apply to every intent equally, so there is
    // no per-call relevance signal to scale it by.
    item.priority = 60;
    item.estimated_tokens = EstimateTokens(item.content);
    items.push_back(std::move(item));
    return items;
}

AISTUDIO_REGISTER_BACKEND(ProjectRulesBackend);

} // namespace aistudio::core
