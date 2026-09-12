#include "Core/Util/Utf8.hpp"

// Vendored header-only third-party library (docs/DEPENDENCY_MANAGEMENT.md).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aistudio::core {

bool IsValidUtf8(std::string_view text) {
    // Delegates to nlohmann's own validator (constructing then dumping a
    // string value exercises the same strict UTF-8 check dump() applies
    // when that string is embedded in a larger JSON document) rather than
    // reimplementing UTF-8 validation -- guarantees this check rejects
    // exactly what would otherwise throw downstream, not an approximation
    // of it.
    try {
        (void)nlohmann::json(std::string(text)).dump();
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

std::size_t Utf8SafeTruncationLength(std::string_view text, std::size_t max_bytes) {
    if (max_bytes >= text.size()) {
        return text.size();
    }
    std::size_t offset = max_bytes;
    const auto is_continuation = [](unsigned char c) { return (c & 0xC0) == 0x80; };
    while (offset > 0 && is_continuation(static_cast<unsigned char>(text[offset]))) {
        --offset;
    }
    return offset;
}

std::wstring Utf8ToWide(std::string_view utf8) {
#if defined(_WIN32)
    if (utf8.empty()) {
        return std::wstring();
    }
    const int wide_length =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wide_length);
    return wide;
#else
    (void)utf8;
    return std::wstring();
#endif
}

std::string WideToUtf8(std::wstring_view wide) {
#if defined(_WIN32)
    if (wide.empty()) {
        return std::string();
    }
    const int utf8_length =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(utf8_length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), utf8_length, nullptr,
                          nullptr);
    return utf8;
#else
    (void)wide;
    return std::string();
#endif
}

std::filesystem::path Utf8ToPath(std::string_view utf8) {
#if defined(_WIN32)
    return std::filesystem::path(Utf8ToWide(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

std::string PathToUtf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    return WideToUtf8(path.wstring());
#else
    return path.string();
#endif
}

std::string PathToUtf8Generic(const std::filesystem::path& path) {
#if defined(_WIN32)
    return WideToUtf8(path.generic_wstring());
#else
    return path.generic_string();
#endif
}

} // namespace aistudio::core
