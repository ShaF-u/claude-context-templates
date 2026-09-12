#include "test_framework.hpp"
#include "Core/Security/Sandbox.hpp"
#include "Core/Util/ProcessRunner.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_sandbox_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempProject() { fs::remove_all(root); }

    void WriteFile(const std::string& relative_path, const std::string& content) const {
        const fs::path full_path = root / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
    }

    [[nodiscard]] std::string RootString() const { return root.string(); }
};

} // namespace

// Broad audit of Sandbox's containment check beyond the specific relative-
// root bug found and fixed earlier this session (docs/ROADMAP.md "Sandbox
// の広範な監査") -- probing attack classes that specific bug fix didn't
// touch: directory junction escapes, case-insensitivity (Windows
// filesystems are case-insensitive; a naive string comparison could
// either wrongly reject a legitimate same-file-different-case path, or
// worse, wrongly ALLOW an escape whose case happens not to match), UNC
// paths, Windows reserved device names, and null-byte injection.
// Verified empirically (see this session's own investigation) that
// std::filesystem::weakly_canonical on this platform already handles
// junctions/case/UNC paths correctly -- those get a permanent regression
// test each below. The two real gaps found, embedded NUL bytes and
// reserved device names, are now rejected outright by Sandbox::Check()
// itself (Core/Security/Sandbox.cpp) rather than relying on
// std::filesystem's own handling (which doesn't treat either specially).
AISTUDIO_TEST(Sandbox_Check_NullByteInPath_IsRejected) {
    TempProject project;
    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(!sandbox.IsAllowed(std::string("a\0b", 3)));
}

AISTUDIO_TEST(Sandbox_Check_JunctionEscapingRoot_IsDenied) {
    TempProject project;
    fs::create_directories(project.root / "inside");
    const fs::path outside = project.root.parent_path() / ("aistudio_sandbox_junction_target_" + project.root.filename().string());
    fs::remove_all(outside);
    fs::create_directories(outside);
    std::ofstream(outside / "secret.txt", std::ios::binary) << "TOP SECRET";

    // Directory junctions don't require the elevated privilege real
    // symlinks do on Windows (std::filesystem::create_symlink would need
    // it) -- `mklink /J` is a cmd.exe builtin, not a standalone exe.
    (void)RunProcess("cmd.exe",
                      {"/c", "mklink", "/J", (project.root / "escape_link").string(), outside.string()}, "");

    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(!sandbox.IsAllowed("escape_link/secret.txt"));

    fs::remove_all(outside);
}

AISTUDIO_TEST(Sandbox_Check_CaseVariants_OfExistingFile_AreAllAllowed) {
    TempProject project;
    project.WriteFile("Docs/Readme.md", "hello\n");
    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(sandbox.IsAllowed("Docs/Readme.md"));
    AISTUDIO_EXPECT(sandbox.IsAllowed("DOCS/README.MD"));
    AISTUDIO_EXPECT(sandbox.IsAllowed("docs/readme.md"));
}

AISTUDIO_TEST(Sandbox_Check_UncPath_IsDenied) {
    TempProject project;
    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(!sandbox.IsAllowed("\\\\localhost\\c$\\Windows\\win.ini"));
}

AISTUDIO_TEST(Sandbox_Check_ReservedDeviceName_IsDenied) {
    TempProject project;
    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(!sandbox.IsAllowed("CON"));
    AISTUDIO_EXPECT(!sandbox.IsAllowed("nul")); // case-insensitive
    AISTUDIO_EXPECT(!sandbox.IsAllowed("sub/NUL.txt")); // reserved regardless of extension or nesting
    AISTUDIO_EXPECT(!sandbox.IsAllowed("COM1"));
}

AISTUDIO_TEST(Sandbox_Check_NameContainingReservedWordAsSubstring_IsAllowed) {
    // "CONFIG" isn't reserved just because it starts with "CON" -- the
    // match is on the whole pre-extension filename, not a substring.
    TempProject project;
    project.WriteFile("CONFIG.txt", "hello\n");
    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(sandbox.IsAllowed("CONFIG.txt"));
}

// Sandbox::Check() must turn EVERY input into a controlled Result<void>,
// never an uncaught exception -- a caller (McpServer's request loop,
// ApiServer's route handlers) that doesn't wrap IsAllowed()/Check() in
// its own try/catch would otherwise crash the whole process on a single
// malformed argument. A bare reserved device name is what this session's
// audit actually found doing this (weakly_canonical() itself threw), so
// it's the concrete regression case, but the assertion is deliberately
// about "never throws" in general, not just this one input.
AISTUDIO_TEST(Sandbox_Check_NeverThrows_EvenForInputsThatMakeFilesystemCallsFail) {
    TempProject project;
    const Sandbox sandbox(project.RootString());
    bool threw = false;
    try {
        (void)sandbox.Check("NUL");
        (void)sandbox.Check("sub/dir/COM3.log");
    } catch (const std::exception&) {
        threw = true;
    }
    AISTUDIO_EXPECT(!threw);
}

AISTUDIO_TEST(Sandbox_Check_NeverThrows_NonAsciiPath) {
    // Regression test for the same class of bug fixed in WorkspaceHash()
    // (docs/ROADMAP.md "非ASCIIパスでWorkspaceHash()がクラッシュ"):
    // fs::path's narrow-string construction on Windows goes through the
    // system ANSI code page (CP_ACP), not UTF-8. On a non-English-default
    // Windows install this codebase's UTF-8 std::string paths can fail to
    // round-trip, throwing rather than just misbehaving.
    TempProject project;
    const Sandbox sandbox(project.RootString());
    bool threw = false;
    try {
        (void)sandbox.Check("\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88.txt"); // 日本語テスト.txt
    } catch (const std::exception&) {
        threw = true;
    }
    AISTUDIO_EXPECT(!threw);
}

AISTUDIO_TEST(Sandbox_Check_PathInsideRoot_IsAllowed) {
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}\n");

    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(sandbox.Check("src/main.cpp"));
}

AISTUDIO_TEST(Sandbox_Check_AbsolutePathInsideRoot_IsAllowed) {
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}\n");

    const Sandbox sandbox(project.RootString());
    const auto absolute_path = (project.root / "src" / "main.cpp").string();
    AISTUDIO_EXPECT(sandbox.Check(absolute_path));
}

AISTUDIO_TEST(Sandbox_Check_PathOutsideRoot_IsDenied) {
    TempProject project;
    project.WriteFile("src/main.cpp", "int main() {}\n");

    const Sandbox sandbox(project.RootString());
    const auto outside_path = (project.root.parent_path() / "outside.txt").string();
    const auto result = sandbox.Check(outside_path);
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(result.Err().code == ErrorCode::PermissionDenied);
}

AISTUDIO_TEST(Sandbox_Check_ParentTraversal_IsDenied) {
    TempProject project;

    const Sandbox sandbox(project.RootString());
    const auto result = sandbox.Check("../outside.txt");
    AISTUDIO_EXPECT(result.IsError());
}

AISTUDIO_TEST(Sandbox_Check_DefaultDenyPattern_BlocksDotGit) {
    TempProject project;
    project.WriteFile(".git/config", "[core]\n");

    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(sandbox.Check(".git/config").IsError());
}

AISTUDIO_TEST(Sandbox_Check_DefaultDenyPattern_BlocksSecretLikeFile) {
    TempProject project;
    project.WriteFile("config/api_secret.txt", "sk-...\n");

    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(!sandbox.IsAllowed("config/api_secret.txt"));
}

AISTUDIO_TEST(Sandbox_Check_OrdinaryFile_IsAllowed) {
    TempProject project;
    project.WriteFile("README.md", "# hello\n");

    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(sandbox.IsAllowed("README.md"));
}

// Regression test for a bug discovered while wiring GitBackend into
// ContextRetriever (docs/ROADMAP.md "MCPモードへBackendRegistryを配線"):
// Sandbox::ResolveAbsolute() used to join a candidate onto the RAW root_
// string (e.g. "." -- the default project_root in both HTTP and MCP
// bootstrap when no aistudio.config sets an absolute one) rather than an
// already-canonicalized absolute root. weakly_canonical() only
// guarantees an absolute result when some prefix of the joined path
// actually exists on disk; a synthetic, nonexistent Backend-provided id
// (nothing under "./git/commit/<sha>" exists as a real file -- this is
// exactly the "Backend-provided id that isn't guaranteed to look like a
// path" case ContextRetriever.hpp's own class comment describes)
// combined with a relative "." root could come back still-relative,
// which then silently failed the containment check against the (always
// absolute) resolved root -- every such id read as "escapes sandbox
// root" even though it plainly doesn't, and ContextRetriever's firewall
// dropped it without any error. Fixed by resolving root_ to absolute
// FIRST and joining the candidate onto that (Core/Security/Sandbox.cpp).
AISTUDIO_TEST(Sandbox_Check_NonExistentSlashId_UnderRelativeDotRoot_IsAllowed) {
    const Sandbox sandbox(".");
    AISTUDIO_EXPECT(sandbox.IsAllowed("git/commit/0000000000000000000000000000000000000000"));
}

// A colon-containing id (a shape this same investigation initially
// suspected of being separately misdetected as an absolute path by
// std::filesystem::path::is_absolute() on Windows) turns out to hit the
// exact same bug above, not a distinct one -- it's allowed once the
// fix above is in place, same as the slash-separated id.
// GitBackend still deliberately avoids colons in its ContextItem ids
// (Core/Git/GitBackend.cpp uses "git/commit/<sha>", not
// "git:commit:<sha>") as defensive practice against Windows' unrelated
// NTFS Alternate Data Stream path syntax ("file:stream"), not because
// Sandbox rejects it.
AISTUDIO_TEST(Sandbox_Check_ColonContainingNonPathId_UnderRelativeDotRoot_IsAllowed) {
    const Sandbox sandbox(".");
    AISTUDIO_EXPECT(sandbox.IsAllowed("git:commit:abc123def456"));
}

AISTUDIO_TEST(Sandbox_AddDenyPattern_ExtendsDefaults) {
    TempProject project;
    project.WriteFile("Assets/locked.bin", "data\n");

    Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(sandbox.IsAllowed("Assets/locked.bin"));

    sandbox.AddDenyPattern("*.bin");
    AISTUDIO_EXPECT(!sandbox.IsAllowed("Assets/locked.bin"));
}

AISTUDIO_TEST(Sandbox_Root_ReturnsConfiguredRoot) {
    const Sandbox sandbox("/some/project/root");
    AISTUDIO_EXPECT(sandbox.Root() == "/some/project/root");
}

AISTUDIO_TEST(Sandbox_Check_RootItself_IsAllowed) {
    TempProject project;
    const Sandbox sandbox(project.RootString());
    AISTUDIO_EXPECT(sandbox.Check("."));
}
