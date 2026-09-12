#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Session/HandoffRecord.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace aistudio::core {

// Actually executes `executable`/`args` in `working_directory` (via
// Core::StartProcess, ProcessRunner.hpp) and blocks up to `timeout` for
// it to finish. This is the only code path in this codebase that
// produces a HandoffVerification's verified_* fields -- an authoring AI
// has no field on HandoffRecord it can write directly to fake a
// Succeeded verification (docs/ROADMAP.md 13-6).
//
// verified_outcome is Succeeded (exit code 0), Failed (nonzero), or
// Unknown if the process was still running when `timeout` elapsed --
// never collapsed into Failed, the same "don't guess a definite outcome
// out of an inconclusive one" principle 13-7 applies to interrupted
// operations. A process left running past timeout is stopped when the
// underlying ManagedProcess goes out of scope (its own destructor
// contract, see ProcessRunner.hpp).
//
// Fails only if the process itself could not be started.
[[nodiscard]] Result<HandoffVerification> RunAndVerifyCommand(
    const std::string& executable, const std::vector<std::string>& args, const std::string& working_directory,
    std::optional<std::chrono::milliseconds> timeout = std::chrono::milliseconds(60000));

} // namespace aistudio::core
