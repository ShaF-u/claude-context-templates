#include "Core/Util/ProcessRunner.hpp"

#include "Core/Util/Utf8.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <mutex>
#include <thread>

namespace aistudio::core {

#if defined(_WIN32)

namespace {

constexpr std::size_t kMaxOutputBytes = 1024 * 1024; // 1 MiB, defensive cap

std::string FormatLastError(DWORD error_code) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (length == 0 || buffer == nullptr) {
        return "error code " + std::to_string(error_code);
    }
    const int utf8_length =
        ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    std::string message(static_cast<std::size_t>(utf8_length), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(length), message.data(), utf8_length, nullptr,
                          nullptr);
    ::LocalFree(buffer);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

// Windows command-line quoting per the algorithm Microsoft documents for
// CommandLineToArgvW's own escaping rules -- doubling backslashes only
// when they immediately precede a quote (literal or closing), so
// arguments like `C:\repo\` or `--grep=foo"bar` round-trip correctly.
std::wstring QuoteArg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring result = L"\"";
    for (auto it = arg.begin();; ++it) {
        std::size_t backslash_count = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslash_count;
        }
        if (it == arg.end()) {
            result.append(backslash_count * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            result.append(backslash_count * 2 + 1, L'\\');
            result.push_back(L'"');
        } else {
            result.append(backslash_count, L'\\');
            result.push_back(*it);
        }
    }
    result.push_back(L'"');
    return result;
}

std::wstring BuildCommandLine(const std::string& executable, const std::vector<std::string>& args) {
    std::wstring command_line = QuoteArg(Utf8ToWide(executable));
    for (const auto& arg : args) {
        command_line.push_back(L' ');
        command_line += QuoteArg(Utf8ToWide(arg));
    }
    return command_line;
}

// Launches `executable` with stdout+stderr redirected to a pipe this
// function's own caller owns (the returned `read_pipe`) and stdin wired
// to NUL, closing every handle that shouldn't outlive this call. Shared
// by RunProcess (which drains read_pipe itself, synchronously) and
// StartProcess (which hands it to ManagedProcess's background reader
// thread instead) -- both need the exact same launch sequence, only what
// happens to the pipe afterward differs.
struct LaunchedProcess {
    HANDLE process = nullptr;
    HANDLE read_pipe = nullptr;
    DWORD pid = 0;
};

Result<LaunchedProcess> LaunchRedirectedProcess(const std::string& executable, const std::vector<std::string>& args,
                                                 const std::string& working_directory) {
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!::CreatePipe(&read_pipe, &write_pipe, &inheritable, 0)) {
        return Result<LaunchedProcess>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "CreatePipe failed: " + FormatLastError(::GetLastError()),
            .module = "Core.Util.ProcessRunner",
        });
    }
    // Only the child's end (write_pipe) should be inherited -- the end we
    // read from ourselves must not leak into the child (standard MSDN
    // "Creating a Child Process with Redirected Input and Output" pattern).
    ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    // Every command this file's callers run is read-only and needs no
    // stdin, but STARTF_USESTDHANDLES requires all three handles to be
    // valid, so hand the child a NUL device rather than leaving it unset.
    const HANDLE stdin_null =
        ::CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING,
                      FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    startup_info.dwFlags = STARTF_USESTDHANDLES;
    startup_info.hStdInput = stdin_null;
    // stdout and stderr share one pipe -- deliberately simpler than two
    // pipes drained on separate threads (which two-pipe redirection
    // requires to avoid deadlock). The commands this is built for
    // (git status/diff/log/show/--version) write nothing to stderr in
    // their normal path, so interleaving isn't a practical concern for
    // this first version; splitting them is a known possible follow-up
    // if that ever stops being true.
    startup_info.hStdOutput = write_pipe;
    startup_info.hStdError = write_pipe;

    std::wstring command_line = BuildCommandLine(executable, args); // CreateProcessW needs a writable buffer
    const std::wstring wide_working_directory = Utf8ToWide(working_directory);

    PROCESS_INFORMATION process_info{};
    const BOOL started = ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE,
                                           0, nullptr, working_directory.empty() ? nullptr : wide_working_directory.c_str(),
                                           &startup_info, &process_info);
    const DWORD create_process_error = started ? 0 : ::GetLastError();

    // The child (if it started) now owns its own copies of these handles;
    // the parent's copies must close so a read on read_pipe actually sees
    // EOF once the child exits instead of blocking forever.
    ::CloseHandle(write_pipe);
    if (stdin_null != INVALID_HANDLE_VALUE) {
        ::CloseHandle(stdin_null);
    }

    if (!started) {
        ::CloseHandle(read_pipe);
        return Result<LaunchedProcess>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "CreateProcessW failed for '" + executable + "': " + FormatLastError(create_process_error),
            .module = "Core.Util.ProcessRunner",
        });
    }
    ::CloseHandle(process_info.hThread);

    return Result<LaunchedProcess>::Ok(LaunchedProcess{process_info.hProcess, read_pipe, process_info.dwProcessId});
}

} // namespace

Result<ProcessResult> RunProcess(const std::string& executable, const std::vector<std::string>& args,
                                  const std::string& working_directory) {
    const auto launched = LaunchRedirectedProcess(executable, args, working_directory);
    if (!launched) {
        return Result<ProcessResult>::Fail(launched.Err());
    }

    std::string output;
    char buffer[4096];
    DWORD bytes_read = 0;
    while (::ReadFile(launched.Value().read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
        if (output.size() < kMaxOutputBytes) {
            output.append(buffer, bytes_read);
        }
    }
    ::CloseHandle(launched.Value().read_pipe);

    ::WaitForSingleObject(launched.Value().process, INFINITE);
    DWORD exit_code = 0;
    ::GetExitCodeProcess(launched.Value().process, &exit_code);
    ::CloseHandle(launched.Value().process);

    if (output.size() > kMaxOutputBytes) {
        // Utf8SafeTruncationLength, not a raw resize(kMaxOutputBytes): a
        // subprocess (e.g. `git show` on a commit touching this project's
        // own Japanese-comment source) can produce output that's valid
        // UTF-8 as a whole, and a naive byte-count cut at this shared,
        // lowest-level boundary can split a multi-byte character for
        // every caller at once (docs/ROADMAP.md CE-5).
        output.resize(Utf8SafeTruncationLength(output, kMaxOutputBytes));
    }

    return Result<ProcessResult>::Ok(ProcessResult{static_cast<int>(exit_code), std::move(output)});
}

struct ManagedProcess::Impl {
    HANDLE process = nullptr;
    HANDLE read_pipe = nullptr;
    DWORD pid = 0;
    std::thread reader_thread;

    mutable std::mutex output_mutex;
    std::string output;

    std::optional<int> cached_exit_code;

    ~Impl() {
        if (process == nullptr) {
            return; // moved-from
        }
        // A process left running (never Wait()'d/Terminate()'d by the
        // caller) is stopped here rather than detached -- an orphaned
        // child outliving this object's owner is a worse default than an
        // explicit, if blunt, cleanup (the same RAII "own it or don't
        // hand it out" reasoning std::jthread's own join-on-destruction
        // follows). Terminate() is a no-op if it already exited.
        StopAndJoin();
        ::CloseHandle(process);
    }

    void StopAndJoin() {
        DWORD exit_code = 0;
        if (::GetExitCodeProcess(process, &exit_code) && exit_code == STILL_ACTIVE) {
            ::TerminateProcess(process, 1);
        }
        // Terminating (or the process exiting on its own) closes its
        // write end of the pipe, so the reader thread's blocking ReadFile
        // loop below observes EOF and returns -- this join is bounded,
        // not a second indefinite wait.
        if (reader_thread.joinable()) {
            reader_thread.join();
        }
    }
};

ManagedProcess::ManagedProcess() : impl_(std::make_unique<Impl>()) {}
ManagedProcess::ManagedProcess(ManagedProcess&&) noexcept = default;
ManagedProcess& ManagedProcess::operator=(ManagedProcess&&) noexcept = default;
ManagedProcess::~ManagedProcess() = default;

std::uint32_t ManagedProcess::Pid() const {
    return static_cast<std::uint32_t>(impl_->pid);
}

bool ManagedProcess::IsRunning() const {
    DWORD exit_code = 0;
    return ::GetExitCodeProcess(impl_->process, &exit_code) && exit_code == STILL_ACTIVE;
}

std::optional<int> ManagedProcess::Wait(std::optional<std::chrono::milliseconds> timeout) {
    if (impl_->cached_exit_code.has_value()) {
        return impl_->cached_exit_code;
    }
    const DWORD wait_ms = timeout.has_value() ? static_cast<DWORD>(timeout->count()) : INFINITE;
    if (::WaitForSingleObject(impl_->process, wait_ms) != WAIT_OBJECT_0) {
        return std::nullopt; // timed out, still running
    }
    DWORD exit_code = 0;
    ::GetExitCodeProcess(impl_->process, &exit_code);
    // The process object signaling doesn't guarantee the reader thread
    // has drained the last bytes yet -- join before returning so
    // OutputSoFar() is complete for a caller that just observed exit.
    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.join();
    }
    impl_->cached_exit_code = static_cast<int>(exit_code);
    return impl_->cached_exit_code;
}

void ManagedProcess::Terminate() {
    impl_->StopAndJoin();
}

std::string ManagedProcess::OutputSoFar() const {
    std::lock_guard lock(impl_->output_mutex);
    return impl_->output;
}

Result<ManagedProcess> StartProcess(const std::string& executable, const std::vector<std::string>& args,
                                     const std::string& working_directory) {
    auto launched = LaunchRedirectedProcess(executable, args, working_directory);
    if (!launched) {
        return Result<ManagedProcess>::Fail(launched.Err());
    }

    ManagedProcess managed;
    managed.impl_->process = launched.Value().process;
    managed.impl_->read_pipe = launched.Value().read_pipe;
    managed.impl_->pid = launched.Value().pid;

    // Draining starts immediately on a background thread, not lazily on
    // first Wait()/OutputSoFar() -- an anonymous pipe's OS-side buffer is
    // small (a handful of KiB), and once it fills, the child blocks on
    // its own write() until someone reads, which for a genuinely
    // long-running process (this class's whole reason to exist) could be
    // an arbitrarily long time after StartProcess() returns.
    ManagedProcess::Impl* impl = managed.impl_.get();
    impl->reader_thread = std::thread([impl] {
        char buffer[4096];
        DWORD bytes_read = 0;
        while (::ReadFile(impl->read_pipe, buffer, sizeof(buffer), &bytes_read, nullptr) && bytes_read > 0) {
            std::lock_guard lock(impl->output_mutex);
            if (impl->output.size() < kMaxOutputBytes) {
                impl->output.append(buffer, bytes_read);
            }
        }
        ::CloseHandle(impl->read_pipe);
        std::lock_guard lock(impl->output_mutex);
        if (impl->output.size() > kMaxOutputBytes) {
            // Same UTF-8-safe cap as RunProcess -- see its own comment.
            impl->output.resize(Utf8SafeTruncationLength(impl->output, kMaxOutputBytes));
        }
    });

    return Result<ManagedProcess>::Ok(std::move(managed));
}

#else

Result<ProcessResult> RunProcess(const std::string& executable, const std::vector<std::string>&, const std::string&) {
    return Result<ProcessResult>::Fail(Error{
        .code = ErrorCode::Internal,
        .message = "RunProcess is not implemented on this platform: " + executable,
        .module = "Core.Util.ProcessRunner",
    });
}

struct ManagedProcess::Impl {};

ManagedProcess::ManagedProcess() : impl_(std::make_unique<Impl>()) {}
ManagedProcess::ManagedProcess(ManagedProcess&&) noexcept = default;
ManagedProcess& ManagedProcess::operator=(ManagedProcess&&) noexcept = default;
ManagedProcess::~ManagedProcess() = default;

std::uint32_t ManagedProcess::Pid() const { return 0; }
bool ManagedProcess::IsRunning() const { return false; }
std::optional<int> ManagedProcess::Wait(std::optional<std::chrono::milliseconds>) { return std::nullopt; }
void ManagedProcess::Terminate() {}
std::string ManagedProcess::OutputSoFar() const { return {}; }

Result<ManagedProcess> StartProcess(const std::string& executable, const std::vector<std::string>&,
                                     const std::string&) {
    return Result<ManagedProcess>::Fail(Error{
        .code = ErrorCode::Internal,
        .message = "StartProcess is not implemented on this platform: " + executable,
        .module = "Core.Util.ProcessRunner",
    });
}

#endif

} // namespace aistudio::core
