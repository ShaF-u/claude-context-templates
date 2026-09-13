#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aistudio::core {

// True if `text` is valid UTF-8. `Core/Search/KeywordSearch`,
// `Core/Context/FileProviderBackend`, and `Core/MCP/McpServer`'s
// context_fetch all read arbitrary project files as raw bytes and embed
// them as JSON string content -- `nlohmann::json::dump()` throws on
// invalid UTF-8, which without this check turns one binary file (an
// image, a .jar, any compiled asset -- FileScanner has no text/binary
// distinction) into a hard failure of the whole response, not just that
// file. Callers use this to skip/reject such content instead.
[[nodiscard]] bool IsValidUtf8(std::string_view text);

// Best-effort Shift-JIS (CP932) -> UTF-8 fallback for bytes that already
// failed IsValidUtf8. Not every project this connects to saves its
// source as UTF-8 -- the GameEngine host repo this was built for
// documents in its own CLAUDE.md that most of its Engine/Source is
// CP932, not UTF-8 (MSVC/Shift-JIS being the long-standing default for a
// Japanese-locale Visual Studio project), which otherwise makes
// context_fetch (McpServer.cpp) reject those files outright as "not
// valid UTF-8 text (binary file?)" even though they're perfectly
// readable text. nullopt when `text` doesn't decode cleanly as CP932
// either (MultiByteToWideChar with MB_ERR_INVALID_CHARS rejects any byte
// sequence with no valid CP932 interpretation) -- callers should keep
// treating that as genuinely binary content, exactly as before this
// existed. Windows-only, mirroring Utf8ToWide/WideToUtf8 below; always
// returns nullopt on non-Windows (nothing in this codebase's non-Windows
// path needs it yet).
[[nodiscard]] std::optional<std::string> Cp932ToUtf8(std::string_view text);

// Largest length <= `max_bytes` that doesn't split a multi-byte UTF-8
// character -- naive byte-count truncation (`text.resize(max_bytes)`) can
// cut a character in half, producing invalid UTF-8 from otherwise-valid
// input (docs/ROADMAP.md CE-5: this exact bug in
// `Core/Git/GitBackend.cpp`'s diff-length cap and
// `Core/Context/ProjectRulesBackend.cpp`'s combined-rules cap crashed
// JSON serialization whenever the cut landed inside e.g. Japanese text).
// Returns text.size() unchanged when max_bytes >= text.size().
[[nodiscard]] std::size_t Utf8SafeTruncationLength(std::string_view text, std::size_t max_bytes);

// UTF-8 -> UTF-16 conversion for Win32 *W APIs (CreateProcessW's
// lpCommandLine/lpCurrentDirectory, etc.). Originally local to
// Core/Util/ProcessRunner.cpp (as WidenUtf8); pulled out here once
// Core/Session/GenericCliAdapter.cpp needed the exact same conversion.
// Empty in -> empty out. Not implemented on non-Windows (returns empty)
// -- nothing in this codebase's non-Windows path needs it yet.
[[nodiscard]] std::wstring Utf8ToWide(std::string_view utf8);

// UTF-16 -> UTF-8 conversion, the reverse of Utf8ToWide -- for turning a
// Win32 *W API's output (e.g. SearchPathW's resolved path) back into the
// UTF-8 std::string this codebase uses everywhere else. Empty in ->
// empty out. Not implemented on non-Windows (returns empty).
[[nodiscard]] std::string WideToUtf8(std::wstring_view wide);

// Constructs a std::filesystem::path from UTF-8 bytes without the
// corruption risk of std::filesystem::path's own narrow-string
// constructor -- on Windows that round-trips through the OS system ANSI
// code page (CP_ACP), NOT UTF-8. For non-ASCII input on a non-English-
// default Windows install this doesn't just risk a silently wrong path;
// confirmed via two real crashes (docs/ROADMAP.md "非ASCIIパスで
// WorkspaceHash()がクラッシュ" and "Sandbox::Check()が非ASCIIパスで
// クラッシュ") it can throw std::system_error outright (a Japanese-locale
// CP_ACP of 932/Shift-JIS can't represent arbitrary UTF-8 byte sequences
// misread as narrow characters). Use this instead of
// std::filesystem::path(utf8_string) anywhere the string might contain
// non-ASCII bytes; pair with WideToUtf8(path.wstring()) or
// WideToUtf8(path.generic_wstring()) for the reverse direction (NOT
// path.string()/.generic_string(), which have the same CP_ACP problem).
// On non-Windows this is just std::filesystem::path(utf8) -- POSIX
// treats paths as raw bytes with no codepage translation to corrupt.
[[nodiscard]] std::filesystem::path Utf8ToPath(std::string_view utf8);

// Reverse of Utf8ToPath, native separators (what path.string() would
// have given on a codebase-consistent UTF-8 build) -- see Utf8ToPath's
// own doc comment for why this must not be path.string() directly.
[[nodiscard]] std::string PathToUtf8(const std::filesystem::path& path);

// Same as PathToUtf8 but forward-slash-normalized (what
// path.generic_string() would have given) -- see Utf8ToPath's own doc
// comment for why this must not be path.generic_string() directly.
[[nodiscard]] std::string PathToUtf8Generic(const std::filesystem::path& path);

} // namespace aistudio::core
