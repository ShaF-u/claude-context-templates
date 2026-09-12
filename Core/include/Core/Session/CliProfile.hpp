#pragma once

#include <string>
#include <utility>
#include <vector>

namespace aistudio::core {

// Launch configuration for one CLI-type AI (docs/ROADMAP.md Phase 13
// "13-1. CLI実行環境の共通インターフェース"). Deliberately separate from
// which model the CLI talks to -- the same "Claude" behaves differently
// launched as a CLI vs. called via API, so this only describes the
// process launch, nothing about the model/provider.
struct CliProfile {
    std::string name; // e.g. "claude" -- identifies which ICliAdapter handles this profile
    std::wstring command_line;
    std::string working_directory;
    // Overlaid onto the launching process's own environment (see
    // PtyProcess::Start()) -- not a full replacement, so PATH/SystemRoot/
    // etc. the CLI needs just to start are still present.
    std::vector<std::pair<std::wstring, std::wstring>> environment_variables;
};

} // namespace aistudio::core
