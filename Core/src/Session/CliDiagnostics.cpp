#include "Core/Session/CliDiagnostics.hpp"

#include "Core/Util/Utf8.hpp"

#include <cwctype>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aistudio::core {

namespace {

namespace fs = std::filesystem;

// Mirrors CreateProcessW's own argv[0] parsing when lpApplicationName is
// null: the module name is everything up to the first whitespace, unless
// it starts with '"', in which case it's everything up to the next '"'
// (CreateProcessW's documented special-casing of the first token, unlike
// later arguments' backslash-escaping rules).
std::wstring ExtractExecutableToken(const std::wstring& command_line) {
    std::size_t i = 0;
    while (i < command_line.size() && std::iswspace(command_line[i])) {
        ++i;
    }
    if (i >= command_line.size()) {
        return L"";
    }
    if (command_line[i] == L'"') {
        const std::size_t start = i + 1;
        const std::size_t end = command_line.find(L'"', start);
        return end == std::wstring::npos ? command_line.substr(start) : command_line.substr(start, end - start);
    }
    const std::size_t start = i;
    const std::size_t end = command_line.find_first_of(L" \t", start);
    return end == std::wstring::npos ? command_line.substr(start) : command_line.substr(start, end - start);
}

// Known gap: a drive-relative token with no separator at all (e.g.
// "C:foo.exe", meaning foo.exe in drive C's current directory) is
// treated as a bare name and searched via SearchPathW below rather than
// resolved as the path it actually is -- vanishingly rare in practice
// (CliProfile::command_line is normally either a bare executable name or
// a full absolute path), not worth special-casing for.
bool HasPathSeparator(const std::wstring& token) {
    return token.find(L'\\') != std::wstring::npos || token.find(L'/') != std::wstring::npos;
}

} // namespace

#if defined(_WIN32)

CliDiagnosticResult DiagnoseCliAvailability(const CliProfile& profile) {
    const std::wstring token = ExtractExecutableToken(profile.command_line);
    if (token.empty()) {
        return CliDiagnosticResult{CliAvailability::NotFound, "", "command_line has no executable token"};
    }

    if (HasPathSeparator(token)) {
        fs::path candidate(token);
        if (!candidate.is_absolute() && !profile.working_directory.empty()) {
            candidate = fs::path(Utf8ToWide(profile.working_directory)) / candidate;
        }
        std::error_code ec;
        if (fs::exists(candidate, ec) && !ec) {
            return CliDiagnosticResult{CliAvailability::Available, WideToUtf8(candidate.wstring()), ""};
        }
        fs::path with_exe = candidate;
        with_exe += L".exe";
        if (fs::exists(with_exe, ec) && !ec) {
            return CliDiagnosticResult{CliAvailability::Available, WideToUtf8(with_exe.wstring()), ""};
        }
        return CliDiagnosticResult{CliAvailability::NotFound, "",
                                    "no file at '" + WideToUtf8(candidate.wstring()) + "' (or with a .exe extension)"};
    }

    // Bare name -- SearchPathW(nullptr, ...) searches the same default
    // locations CreateProcessW does (calling process's own directory,
    // current directory, Windows system/Windows directories, then PATH).
    // lpExtension is only applied when the name itself has none (see this
    // function's own header doc comment for the full caveat list).
    wchar_t buffer[MAX_PATH];
    const DWORD length = ::SearchPathW(nullptr, token.c_str(), L".exe", MAX_PATH, buffer, nullptr);
    if (length > 0 && length < MAX_PATH) {
        return CliDiagnosticResult{CliAvailability::Available, WideToUtf8(std::wstring(buffer, length)), ""};
    }
    return CliDiagnosticResult{CliAvailability::NotFound, "",
                                "'" + WideToUtf8(token) + "' was not found in PATH or the default search locations"};
}

#else

CliDiagnosticResult DiagnoseCliAvailability(const CliProfile&) {
    return CliDiagnosticResult{CliAvailability::Unknown, "", "not implemented on this platform"};
}

#endif

} // namespace aistudio::core
