#pragma once

#include "Core/Session/CliProfile.hpp"

#include <string>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-1. CLI実行環境の共通インターフェース":
// "実行環境の診断 -- 存在確認 / 必要設定 / 起動失敗原因の提示". Unknown
// is a real outcome here, not a fallback -- see DiagnoseCliAvailability's
// own doc comment for when this function actually returns it.
enum class CliAvailability { Unknown, Available, NotFound };

struct CliDiagnosticResult {
    CliAvailability availability = CliAvailability::Unknown;
    // Only set when availability == Available -- the absolute path the
    // executable actually resolved to (useful when `profile` names a
    // bare executable found via PATH, so a caller can show exactly which
    // one would run).
    std::string resolved_path;
    std::string message;
};

// Best-effort preflight check for whether `profile.command_line`'s
// executable can actually be launched, before attempting
// ICliAdapter::Start() -- the complement to Start()'s own
// LastErrorMessage(), which only explains a failure AFTER an attempt.
//
// Deliberately approximate, not a guarantee (AGENT.md #15 -- this is
// honest about its limits, not lazy): it mirrors CreateProcessW's own
// module-name resolution (SearchPathW with a default .exe extension for
// an extensionless bare name, the same search order CreateProcessW uses
// when lpApplicationName is null) closely enough to catch the common
// case ("this CLI isn't installed" / "this path is wrong"), but does NOT
// replicate every nuance of that resolution -- notably, `profile`'s own
// CliProfile::environment_variables can overlay a different PATH onto
// the process CreateProcessW would actually launch with (see
// PtyProcess::Start()), and this check does not use that overlay; it
// checks against the CALLING process's own PATH/search locations, which
// can disagree with what the launched process would actually see. When
// that matters, the result may say NotFound for something that would
// have started (or vice versa) -- Unknown is returned instead of
// guessing at Available/NotFound only when this function's own attempt
// to search the filesystem itself fails unexpectedly (not the "isn't
// installed" case, which is a confident NotFound), and on any platform
// other than Windows (not implemented there).
[[nodiscard]] CliDiagnosticResult DiagnoseCliAvailability(const CliProfile& profile);

} // namespace aistudio::core
