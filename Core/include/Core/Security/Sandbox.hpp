#pragma once

#include "Core/Error/Result.hpp"

#include <string>
#include <vector>

namespace aistudio::core {

// Constrains which filesystem paths an AI-initiated operation may touch
// — docs/MASTER_SPEC.md #73 Security > Sandbox. PermissionPolicy gates
// whether a Command needs human approval by NAME; Sandbox is the
// path-scoped counterpart, checking WHERE an operation may act — a
// filesystem-touching Backend (Git/IDE/Unreal/Unity, Phase 6/7, not
// built yet) is expected to call Check() before writing/deleting/
// reading a path, independently of whatever PermissionPolicy decided
// about the Command itself.
//
// A path is allowed only if it resolves inside `root` — path traversal
// like "../" is resolved via std::filesystem::weakly_canonical before
// the containment check, so it can't escape root by construction — AND
// doesn't match any deny pattern, checked the same way FileScanner
// checks its own ignore rules (FileScanner::IsIgnored, reused directly
// rather than reimplementing glob/segment matching a second time).
class Sandbox {
public:
    struct Options {
        std::vector<std::string> deny_patterns = DefaultDenyPatterns();
    };

    [[nodiscard]] static std::vector<std::string> DefaultDenyPatterns();

    explicit Sandbox(std::string root, Options options = {});

    // Ok if `path` (absolute, or relative to root) is allowed;
    // ErrorCode::PermissionDenied with a reason otherwise.
    [[nodiscard]] Result<void> Check(const std::string& path) const;
    [[nodiscard]] bool IsAllowed(const std::string& path) const;

    void AddDenyPattern(std::string pattern);
    [[nodiscard]] const std::string& Root() const { return root_; }

private:
    std::string root_;
    std::vector<std::string> deny_patterns_;
};

} // namespace aistudio::core
