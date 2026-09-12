#include "Core/Util/WorkspaceHash.hpp"

#include "Core/Util/Utf8.hpp"

#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace aistudio::core {

namespace {

namespace fs = std::filesystem;

// See WorkspaceHash.hpp's doc comment for the full algorithm spec this
// must stay byte-identical to across all four language implementations.
std::string NormalizeForHash(std::string path) {
    for (char& c : path) {
        if (c == '\\') {
            c = '/';
        }
    }
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    for (char& c : path) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return path;
}

std::uint64_t Fnv1a64(const std::string& data) {
    constexpr std::uint64_t kOffsetBasis = 0xcbf29ce484222325ULL;
    constexpr std::uint64_t kPrime = 0x100000001b3ULL;
    std::uint64_t hash = kOffsetBasis;
    for (unsigned char byte : data) {
        hash ^= static_cast<std::uint64_t>(byte);
        hash *= kPrime;
    }
    return hash;
}

} // namespace

std::string WorkspaceHash(const std::string& workspace_root) {
    if (workspace_root.empty()) {
        return "";
    }
    std::error_code ec;
#if defined(_WIN32)
    // fs::path's narrow-string constructor/generic_string() round-trip
    // through the OS's system ANSI code page (CP_ACP), NOT UTF-8 -- on a
    // non-English-default Windows install (e.g. Japanese, CP_ACP=932
    // Shift-JIS) this doesn't just produce a wrong hash for non-ASCII
    // `workspace_root` input, it can throw
    // (std::system_error "no mapping for the Unicode character exists in
    // the target multi-byte code page") when the UTF-8 bytes this
    // codebase always uses for std::string happen to not form a valid
    // CP_ACP byte sequence. Go through the wide string explicitly
    // instead (Utf8ToWide/WideToUtf8, already used elsewhere in Core for
    // exactly this Windows *W-API reason) so the conversion is UTF-8 on
    // both ends, matching this function's own documented "hash the UTF-8
    // bytes" contract and the three IDE-extension ports of it.
    const auto canonical = fs::weakly_canonical(fs::path(Utf8ToWide(workspace_root)), ec);
    const std::string normalized_generic = ec ? std::string() : WideToUtf8(canonical.generic_wstring());
#else
    const auto canonical = fs::weakly_canonical(fs::path(workspace_root), ec);
    const std::string normalized_generic = ec ? std::string() : canonical.generic_string();
#endif
    if (ec) {
        return "";
    }

    const std::string normalized = NormalizeForHash(normalized_generic);
    const std::uint64_t hash = Fnv1a64(normalized);

    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << hash;
    return out.str();
}

} // namespace aistudio::core
