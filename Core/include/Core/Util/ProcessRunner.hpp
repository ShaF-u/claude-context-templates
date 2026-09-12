#pragma once

#include "Core/Error/Result.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aistudio::core {

struct ProcessResult {
    int exit_code = 0;
    // stdout and stderr merged into one buffer (see RunProcess's .cpp
    // comment for why) -- a hard 1 MiB cap is applied defensively.
    std::string output;
};

// Runs `executable` with `args` in `working_directory` (empty = current
// directory), waits for it to exit, and captures its output. Never goes
// through a shell (cmd.exe /c or similar) -- the command line is built by
// quoting each argument individually, so there is no shell-injection
// surface regardless of what `args` contains.
//
// Fails only if the process itself couldn't be started (executable not
// found, etc.) -- a non-zero exit code is still Ok(), reported via
// ProcessResult::exit_code, since "the tool ran and reported an error" is
// a normal outcome callers need to branch on, not a RunProcess-level
// failure.
[[nodiscard]] Result<ProcessResult> RunProcess(const std::string& executable, const std::vector<std::string>& args,
                                                const std::string& working_directory);

// Non-blocking counterpart to RunProcess (docs/ROADMAP.md Phase 13 "着手
// 前に潰すべき前提": RunProcess's complete synchronicity and lack of a
// PID/handle/cancellation token make it unusable for supervising a
// long-running process). StartProcess() returns as soon as the child
// exists; the returned ManagedProcess exposes its PID immediately, can be
// polled/waited on the caller's own schedule, and can be Terminate()'d.
// Output is drained continuously on a background thread from the moment
// the process starts (an unread anonymous pipe fills and stalls the
// child once its OS buffer is full, so this can't be deferred to Wait()
// the way RunProcess's single blocking read loop gets away with) -- the
// same 1 MiB defensive cap and UTF-8-safe truncation as RunProcess.
//
// Deliberately NOT a full replacement for RunProcess: no progress
// callback, no split stdout/stderr. This exists to satisfy exactly what
// Phase 13's session supervision needs (PID, running/exited, wait with a
// timeout, terminate) -- see that section for the actual consumer this
// is built for, not yet wired up here (AGENT.md #14: this file adds only
// what's needed to unblock Phase 13, not a speculative general-purpose
// process API).
class ManagedProcess {
public:
    ManagedProcess(ManagedProcess&&) noexcept;
    ManagedProcess& operator=(ManagedProcess&&) noexcept;
    ManagedProcess(const ManagedProcess&) = delete;
    ManagedProcess& operator=(const ManagedProcess&) = delete;
    ~ManagedProcess();

    [[nodiscard]] std::uint32_t Pid() const;

    // True if the process has not yet exited, as of this call (a
    // snapshot, not a guarantee -- the process can exit immediately
    // after this returns, same caveat every "is it still running" check
    // over an external process has).
    [[nodiscard]] bool IsRunning() const;

    // Blocks until the process exits, or `timeout` elapses (nullopt =
    // wait forever, matching RunProcess's own unconditional wait).
    // Returns the exit code once observed, cached for later calls;
    // nullopt if `timeout` elapsed first (still running).
    [[nodiscard]] std::optional<int> Wait(std::optional<std::chrono::milliseconds> timeout = std::nullopt);

    // Best-effort immediate stop. Windows has no graceful signal for an
    // arbitrary child process (no SIGTERM equivalent) -- this is
    // TerminateProcess, the same blunt tool GitBackend's own comments
    // already note the platform only offers. A no-op if already exited.
    void Terminate();

    // Output captured so far -- safe to call whether the process is
    // still running or has already exited.
    [[nodiscard]] std::string OutputSoFar() const;

private:
    friend Result<ManagedProcess> StartProcess(const std::string&, const std::vector<std::string>&,
                                                const std::string&);
    ManagedProcess();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Result<ManagedProcess> StartProcess(const std::string& executable, const std::vector<std::string>& args,
                                                    const std::string& working_directory);

} // namespace aistudio::core
