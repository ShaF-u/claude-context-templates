#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aistudio::core {

// Owns a Windows ConPTY pseudo console and the child process behind it --
// the process-supervision half of Gui/src/Terminal.{hpp,cpp}'s embedded
// terminal, moved here per docs/ROADMAP.md Phase 13's prerequisite
// "ConPTYがGui側にある" (AGENT.md #2: Core doesn't know about GUI, GUI
// depends on Core -- session/process supervision is Core's
// responsibility, not the renderer's). What stays in Gui: VT100/ANSI
// parsing into the on-screen cell grid, ImGui rendering, and keystroke
// capture -- this class has no notion of terminal semantics (colors,
// cursor, escape sequences) at all, only raw bytes in (WriteInput) and
// out (TakeOutput()).
//
// PImpl (no <windows.h> in this header) so every other Core translation
// unit that transitively includes this file doesn't inherit ConPTY/Win32
// types -- same reasoning as ProcessRunner.hpp's ManagedProcess.
//
// Output is drained continuously on a background reader thread from the
// moment the child starts (an unread ConPTY output pipe fills and stalls
// the child once its OS buffer is full -- same reasoning as
// ProcessRunner.hpp's StartProcess()/ManagedProcess) into a mutex-
// protected buffer; TakeOutput() drains and clears it -- safe to call
// from any thread, including once per UI frame (its actual call site,
// Gui/src/Terminal.cpp's Draw(), which previously ran the VT parser
// directly on this reader thread -- now it runs once per frame on
// whatever thread calls TakeOutput(), a behavior-preserving change since
// the terminal panel only ever visibly redraws once per frame anyway).
class PtyProcess {
public:
    PtyProcess();
    ~PtyProcess();

    // Non-movable, same as the Gui::Terminal class this was extracted
    // from -- always used as a single long-lived member/global, never
    // passed by value; the reader thread's Impl* would need adjusting on
    // move otherwise (it doesn't take one today because there's no real
    // use for it here).
    PtyProcess(const PtyProcess&) = delete;
    PtyProcess& operator=(const PtyProcess&) = delete;
    PtyProcess(PtyProcess&&) = delete;
    PtyProcess& operator=(PtyProcess&&) = delete;

    // Spawns `command_line` (e.g. L"cmd.exe") behind a new pseudo console
    // sized `cols`x`rows`. Returns false (see LastErrorMessage()) on
    // failure -- never throws, matching every other OS-interaction
    // primitive in this codebase (ProcessRunner, FileWatcher). A no-op
    // (returns false) if already running.
    // `extra_environment` overlays onto (doesn't replace) this process's
    // own inherited environment block -- name matches are overridden,
    // everything else (PATH, SystemRoot, ...) passes through unchanged,
    // so an arbitrary CLI still gets what it needs to even start.
    // `working_directory` empty (default) inherits this process's own
    // current directory, matching the prior no-argument behavior.
    [[nodiscard]] bool Start(const std::wstring& command_line, std::int16_t cols = 120, std::int16_t rows = 32,
                              const std::vector<std::pair<std::wstring, std::wstring>>& extra_environment = {},
                              const std::wstring& working_directory = L"");

    // Terminates the child (if still running) and releases the pseudo
    // console/pipes/reader thread. Safe to call when not running; the
    // destructor calls this automatically if not already stopped.
    void Stop();

    // Tells the pseudo console the child's visible terminal size changed.
    // No-op if not running.
    void Resize(std::int16_t cols, std::int16_t rows);

    // ReadFile() on the output pipe doesn't reliably EOF just because the
    // child process exited -- ConPTY can keep the pipe open after the
    // last attached process detaches. This checks GetExitCodeProcess()
    // (the authoritative "is it actually still running" signal) as a
    // side effect, same as Gui/src/Terminal.cpp's old RefreshRunningState()
    // did -- intended to be called once per UI frame, same call pattern.
    [[nodiscard]] bool IsRunning();

    // Set once IsRunning() has observed the child actually exit (either
    // on its own or via Stop()/TerminateProcess, which always yields 0).
    // nullopt before that -- still running, or never started.
    [[nodiscard]] std::optional<int> ExitCode() const;

    [[nodiscard]] const std::string& LastErrorMessage() const;

    // Raw bytes to the child's stdin (keystrokes, VT sequences the
    // caller already encoded, e.g. arrow-key CSI sequences). A no-op if
    // not running.
    void WriteInput(std::string_view bytes);

    // Returns and clears whatever output bytes the reader thread has
    // accumulated since the last call. Never blocks.
    [[nodiscard]] std::string TakeOutput();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aistudio::core
