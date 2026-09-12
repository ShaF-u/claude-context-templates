#include "Core/Security/Sandbox.hpp"

#include "Core/Project/FileScanner.hpp"
#include "Core/Util/Utf8.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>

namespace aistudio::core {

namespace {

namespace fs = std::filesystem;

// Windows treats these as reserved device names for ANY file component
// that matches one case-insensitively up to (not including) its first
// '.' -- "NUL", "nul.txt", and "Nul.tar.gz" all address the NUL device,
// never a real file, regardless of directory. Opening one can't leak
// arbitrary file content (it's still nominally within root, not an
// escape), but could hang or misbehave if some caller ever tries to
// actually read/write through a Sandbox-approved path -- rejected here
// as a narrow, cheap defensive measure alongside the real containment
// check below.
bool IsReservedDeviceName(const fs::path& resolved_path) {
    static constexpr std::array<const char*, 24> kReserved = {
        "CON",  "PRN",  "AUX",  "NUL",  "COM0", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7",
        "COM8", "COM9", "LPT0", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    };
    const std::string filename = PathToUtf8(resolved_path.filename());
    const std::string base = filename.substr(0, filename.find('.'));
    std::string upper = base;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return std::find(kReserved.begin(), kReserved.end(), upper) != kReserved.end();
}

// `root` must already be an absolute, weakly_canonical'd path -- NOT the
// raw (possibly relative, e.g. ".") root_ string. weakly_canonical() only
// guarantees an absolute result when it can find SOME existing prefix to
// canonicalize; for a synthetic, nonexistent candidate (a Backend-provided
// id, not a real file -- e.g. GitBackend's "git/commit/<sha>") joined onto
// a relative root like ".", nothing in the joined path exists on disk, so
// weakly_canonical can return it still relative instead of absolute --
// silently breaking the containment check below (an absolute resolved_root
// can never component-wise match a still-relative resolved_path, so every
// such id reads as "escapes sandbox root" even though it doesn't). Joining
// onto an already-absolute root sidesteps this entirely: the result is
// absolute by construction regardless of whether any part of `path` exists.
fs::path ResolveAbsolute(const fs::path& root, const fs::path& candidate_in) {
    fs::path candidate = candidate_in;
    if (!candidate.is_absolute()) {
        candidate = root / candidate;
    }
    return fs::weakly_canonical(candidate);
}

// std::filesystem's own containment check: compares path components
// (not a raw string prefix — "C:/Foo" must not appear "inside"
// "C:/FooBar" just because one string prefixes the other).
bool IsWithinRoot(const fs::path& resolved_root, const fs::path& resolved_path) {
    auto root_it = resolved_root.begin();
    auto path_it = resolved_path.begin();
    for (; root_it != resolved_root.end(); ++root_it, ++path_it) {
        if (path_it == resolved_path.end() || *path_it != *root_it) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::string> Sandbox::DefaultDenyPatterns() {
    return {
        ".git", ".ssh", ".env", "*.pem", "*.key", "*.pfx", "id_rsa*", "*secret*",
    };
}

Sandbox::Sandbox(std::string root, Options options) : root_(std::move(root)), deny_patterns_(std::move(options.deny_patterns)) {}

void Sandbox::AddDenyPattern(std::string pattern) {
    deny_patterns_.push_back(std::move(pattern));
}

Result<void> Sandbox::Check(const std::string& path) const {
    // Defense in depth against null-byte injection: std::string/
    // std::filesystem::path are length-aware and treat an embedded NUL
    // as an ordinary character throughout the containment check below,
    // but some later consumer of an approved path could, on Windows,
    // convert it to a null-terminated buffer for a raw Win32 call (the
    // conversion CreateFileW's own LPCWSTR contract implies) -- which
    // would silently truncate at the first NUL, opening a shorter path
    // than the one actually validated here. Rejecting outright is
    // simpler and safer than trying to reason about every current and
    // future caller's own string handling.
    if (path.find('\0') != std::string::npos) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::PermissionDenied,
            .message = "path contains an embedded null byte",
            .module = "Core.Security.Sandbox",
        });
    }

    // Everything below touches std::filesystem, all the way from
    // constructing the very first fs::path -- fs::path's own narrow-
    // string constructor/string()/generic_string() round-trip through
    // the OS system ANSI code page (CP_ACP) on Windows, NOT UTF-8, which
    // this codebase's std::string always is; confirmed via a real crash
    // that non-ASCII input can throw std::system_error at construction,
    // not just misresolve (docs/ROADMAP.md "Sandbox::Check()が非ASCIIパス
    // でクラッシュ"). Utf8ToPath()/PathToUtf8() (Core/Util/Utf8.hpp) avoid
    // that specific failure mode, but this whole block still runs inside
    // one try/catch as defense in depth for any OTHER OS-specific
    // filesystem exception this file's audit didn't specifically
    // anticipate -- Sandbox exists to turn "is this path allowed" into a
    // controlled Result<void>, so nothing past this point may let an
    // exception escape uncaught (a caller like McpServer/ApiServer's
    // request loop would otherwise crash the whole process on one
    // malformed argument).
    try {
        const fs::path candidate_path = Utf8ToPath(path);

        // Checked here, before any weakly_canonical() call touches it
        // below -- confirmed empirically that MSVC's std::filesystem
        // throws a filesystem_error ("The parameter is incorrect") when
        // weakly_canonical() tries to probe a reserved device name's
        // existence, rather than just reporting "not found". Rejecting
        // the lexical filename first (a pure string operation, no
        // filesystem access, can't throw) avoids ever reaching that call
        // for the common case (a bare reserved name or one with an
        // extension).
        if (IsReservedDeviceName(candidate_path)) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::PermissionDenied,
                .message = "path resolves to a reserved Windows device name: " + path,
                .module = "Core.Security.Sandbox",
            });
        }

        const auto resolved_root = fs::weakly_canonical(Utf8ToPath(root_));
        const auto resolved_path = ResolveAbsolute(resolved_root, candidate_path);

        if (!IsWithinRoot(resolved_root, resolved_path)) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::PermissionDenied,
                .message = "path escapes sandbox root '" + root_ + "': " + path,
                .module = "Core.Security.Sandbox",
            });
        }

        std::error_code ec;
        const auto relative_path = fs::relative(resolved_path, resolved_root, ec);
        const FileScanner deny_matcher(FileScanner::Options{.ignore_patterns = deny_patterns_});
        if (!ec && deny_matcher.IsIgnored(PathToUtf8Generic(relative_path))) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::PermissionDenied,
                .message = "path matches a sandbox deny pattern: " + path,
                .module = "Core.Security.Sandbox",
            });
        }

        return Result<void>::Ok();
    } catch (const std::exception& e) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::PermissionDenied,
            .message = "path resolution failed: " + std::string(e.what()),
            .module = "Core.Security.Sandbox",
        });
    }
}

bool Sandbox::IsAllowed(const std::string& path) const {
    return static_cast<bool>(Check(path));
}

} // namespace aistudio::core
