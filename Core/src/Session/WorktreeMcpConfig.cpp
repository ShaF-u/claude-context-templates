#include "Core/Session/WorktreeMcpConfig.hpp"

#include "Core/Util/Utf8.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <filesystem>
#include <fstream>

namespace aistudio::core {

namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
} // namespace

Result<void> WriteWorktreeMcpConfig(const std::string& worktree_path, const std::string& core_cli_absolute_path) {
    if (worktree_path.empty() || core_cli_absolute_path.empty()) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                         .message = "worktree_path and core_cli_absolute_path must both be non-empty",
                                         .module = "Core.Session.WorktreeMcpConfig"});
    }
    std::error_code exists_ec;
    if (!fs::is_directory(Utf8ToPath(worktree_path), exists_ec) || exists_ec) {
        return Result<void>::Fail(Error{.code = ErrorCode::NotFound,
                                         .message = "worktree_path does not exist or is not a directory: " + worktree_path,
                                         .module = "Core.Session.WorktreeMcpConfig"});
    }

    // Matches the project's own root .mcp.json shape exactly (see
    // .mcp.json), just with an absolute "command" instead of that file's
    // relative one -- a worktree has no build output of its own to
    // resolve a relative path against. Built incrementally (rather than
    // one nested brace-init literal) to avoid nlohmann::json's own
    // documented array-vs-object initializer-list ambiguity.
    Json server;
    server["type"] = "stdio";
    server["command"] = core_cli_absolute_path;
    server["args"] = Json::array({"--mcp"});
    server["env"] = Json::object();

    Json config;
    config["mcpServers"]["aistudio-core"] = server;

    const fs::path config_path = Utf8ToPath(worktree_path) / ".mcp.json";
    std::ofstream out(config_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return Result<void>::Fail(Error{.code = ErrorCode::IOError,
                                         .message = "failed to open for writing: " + PathToUtf8(config_path),
                                         .module = "Core.Session.WorktreeMcpConfig"});
    }
    out << config.dump(2);
    if (!out) {
        return Result<void>::Fail(Error{.code = ErrorCode::IOError,
                                         .message = "failed to write: " + PathToUtf8(config_path),
                                         .module = "Core.Session.WorktreeMcpConfig"});
    }
    return Result<void>::Ok();
}

} // namespace aistudio::core
