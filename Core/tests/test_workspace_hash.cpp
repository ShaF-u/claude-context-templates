#include "test_framework.hpp"
#include "Core/Util/WorkspaceHash.hpp"

#include <filesystem>

using namespace aistudio::core;

AISTUDIO_TEST(WorkspaceHash_EmptyInput_ReturnsEmpty) {
    AISTUDIO_EXPECT(WorkspaceHash("").empty());
}

AISTUDIO_TEST(WorkspaceHash_IsDeterministic) {
    const std::string root = std::filesystem::temp_directory_path().string();
    const auto first = WorkspaceHash(root);
    const auto second = WorkspaceHash(root);
    AISTUDIO_EXPECT(!first.empty());
    AISTUDIO_EXPECT(first == second);
}

AISTUDIO_TEST(WorkspaceHash_IsSixteenLowercaseHexCharacters) {
    const std::string root = std::filesystem::temp_directory_path().string();
    const auto hash = WorkspaceHash(root);
    AISTUDIO_EXPECT(hash.size() == 16);
    for (char c : hash) {
        AISTUDIO_EXPECT((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    }
}

AISTUDIO_TEST(WorkspaceHash_DifferentDirectories_ProduceDifferentHashes) {
    const std::string temp_dir = std::filesystem::temp_directory_path().string();
    const std::string current_dir = std::filesystem::current_path().string();
    AISTUDIO_EXPECT(WorkspaceHash(temp_dir) != WorkspaceHash(current_dir));
}

AISTUDIO_TEST(WorkspaceHash_TrailingSlashVariants_ProduceSameHash) {
    const std::string root = std::filesystem::temp_directory_path().string();
    std::string with_trailing_slash = root;
    if (!with_trailing_slash.empty() && with_trailing_slash.back() != '/' && with_trailing_slash.back() != '\\') {
        with_trailing_slash.push_back('/');
    }
    AISTUDIO_EXPECT(WorkspaceHash(root) == WorkspaceHash(with_trailing_slash));
}

AISTUDIO_TEST(WorkspaceHash_BackslashAndForwardSlashVariants_ProduceSameHash) {
    const std::string root = std::filesystem::temp_directory_path().string();
    std::string forward_slashes = root;
    for (char& c : forward_slashes) {
        if (c == '\\') {
            c = '/';
        }
    }
    AISTUDIO_EXPECT(WorkspaceHash(root) == WorkspaceHash(forward_slashes));
}

AISTUDIO_TEST(WorkspaceHash_CaseVariants_ProduceSameHash) {
    const std::string root = std::filesystem::temp_directory_path().string();
    std::string uppercased = root;
    for (char& c : uppercased) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    AISTUDIO_EXPECT(WorkspaceHash(root) == WorkspaceHash(uppercased));
}

#if defined(_WIN32)
AISTUDIO_TEST(WorkspaceHash_KnownVector_MatchesIndependentlyComputedFnv1a) {
    // Cross-language pin: this exact value was independently computed
    // with a standalone Node.js script implementing the same algorithm
    // (FNV-1a 64-bit over the normalized, lowercased, forward-slashed
    // path) as Tools/vscode-extension/src/editorState.ts's own
    // workspaceHash(). A nonexistent absolute path is used deliberately
    // -- weakly_canonical only lexically normalizes a path with no
    // existing components (no disk-dependent case/symlink resolution),
    // so this value is reproducible on any machine, unlike a test built
    // from a real (and therefore environment-specific) directory.
    AISTUDIO_EXPECT(WorkspaceHash("C:\\NonexistentAiStudioTestPath12345\\sub") == "e7ad9045462c3553");
    AISTUDIO_EXPECT(WorkspaceHash("C:/NonexistentAiStudioTestPath12345/sub") == "e7ad9045462c3553");
}

AISTUDIO_TEST(WorkspaceHash_NonAsciiPath_MatchesIndependentlyComputedFnv1a) {
    // Cross-language pin (same methodology as the ASCII vector above),
    // independently computed via Node.js for this exact UTF-8 input
    // string. Regression test for a real bug found by review + confirmed
    // by running it before the fix: fs::path's narrow-string round-trip
    // on Windows goes through the system ANSI code page (CP_ACP), NOT
    // UTF-8 -- on this machine (CP_ACP=932 Shift-JIS) that didn't just
    // produce a wrong hash for non-ASCII input, it THREW
    // (std::system_error, "no mapping for the Unicode character exists
    // in the target multi-byte code page") the moment WorkspaceHash() was
    // called with a workspace_root containing Japanese characters --
    // e.g. any project living under a non-ASCII Windows user profile
    // path. Fixed by routing through Utf8ToWide/WideToUtf8 explicitly
    // (see WorkspaceHash.cpp) instead of relying on fs::path's own
    // narrow<->wide conversion.
    const std::string result = WorkspaceHash("C:\\Users\\\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88\\\xe3\x83\x97\xe3\x83\xad\xe3\x82\xb8\xe3\x82\xa7\xe3\x82\xaf\xe3\x83\x88");
    AISTUDIO_EXPECT(result == "0b0e5cfeb6b664d8");
}
#endif

AISTUDIO_TEST(WorkspaceHash_RelativePath_ResolvesAgainstCurrentDirectory) {
    // Core/src/main.cpp's `project.root` config defaults to "." -- the
    // hash must resolve that the same way weakly_canonical resolves it
    // for WorkspaceRootMismatch(), or the filename Core computes for its
    // own process would never match the (always-absolute) one an IDE
    // extension writes.
    const std::string current_dir = std::filesystem::current_path().string();
    AISTUDIO_EXPECT(WorkspaceHash(".") == WorkspaceHash(current_dir));
}
