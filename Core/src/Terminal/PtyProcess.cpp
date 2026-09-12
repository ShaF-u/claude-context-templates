#include "Core/Terminal/PtyProcess.hpp"

#include "Core/Logging/Logger.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <thread>

namespace aistudio::core {

#if defined(_WIN32)

namespace {

// Win32's lpEnvironment wants one contiguous NAME=VALUE\0 ... \0\0 block.
// Starting from GetEnvironmentStringsW() (this process's own block) rather
// than only `extra` is what lets an arbitrary CLI still find PATH/
// SystemRoot/etc. it needs just to start.
std::vector<wchar_t> BuildEnvironmentBlock(const std::vector<std::pair<std::wstring, std::wstring>>& extra) {
    std::vector<std::pair<std::wstring, std::wstring>> vars;
    if (LPWCH base = ::GetEnvironmentStringsW(); base != nullptr) {
        for (const wchar_t* p = base; *p != L'\0'; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const size_t eq = entry.find(L'=');
            // Skip malformed/per-drive-current-directory entries
            // (Windows writes these as e.g. "=C:=C:\\some\\dir").
            if (eq == std::wstring::npos || eq == 0) {
                continue;
            }
            vars.emplace_back(entry.substr(0, eq), entry.substr(eq + 1));
        }
        ::FreeEnvironmentStringsW(base);
    }
    for (const auto& [name, value] : extra) {
        const auto it = std::find_if(vars.begin(), vars.end(), [&](const auto& v) {
            return _wcsicmp(v.first.c_str(), name.c_str()) == 0;
        });
        if (it != vars.end()) {
            it->second = value;
        } else {
            vars.emplace_back(name, value);
        }
    }

    std::vector<wchar_t> block;
    for (const auto& [name, value] : vars) {
        const std::wstring entry = name + L"=" + value;
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

} // namespace

struct PtyProcess::Impl {
    HPCON pseudo_console = nullptr;
    HANDLE input_write = nullptr;  // our end; ConPTY reads the child's stdin from the other end
    HANDLE output_read = nullptr;  // our end; ConPTY writes the child's stdout to the other end
    PROCESS_INFORMATION process_info{};
    LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = nullptr;

    std::thread reader_thread;
    std::atomic<bool> running{false};

    std::mutex output_mutex;
    std::string pending_output; // raw bytes accumulated by the reader thread, drained by TakeOutput()

    std::string last_error;
    std::optional<int> exit_code;

    void CleanupHandles() {
        if (input_write != nullptr) {
            ::CloseHandle(input_write);
            input_write = nullptr;
        }
        if (output_read != nullptr) {
            ::CloseHandle(output_read);
            output_read = nullptr;
        }
        if (pseudo_console != nullptr) {
            ::ClosePseudoConsole(pseudo_console);
            pseudo_console = nullptr;
        }
        if (attribute_list != nullptr) {
            ::DeleteProcThreadAttributeList(attribute_list);
            ::HeapFree(::GetProcessHeap(), 0, attribute_list);
            attribute_list = nullptr;
        }
    }

    void ReaderThreadMain() {
        char buf[4096];
        while (true) {
            DWORD bytes_read = 0;
            const BOOL ok = ::ReadFile(output_read, buf, sizeof(buf), &bytes_read, nullptr);
            if (!ok || bytes_read == 0) {
                break; // pipe closed, or the child process exited
            }
            std::lock_guard lock(output_mutex);
            pending_output.append(buf, bytes_read);
        }
        running = false;
    }
};

PtyProcess::PtyProcess() : impl_(std::make_unique<Impl>()) {}

PtyProcess::~PtyProcess() {
    Stop();
}

bool PtyProcess::Start(const std::wstring& command_line, std::int16_t cols, std::int16_t rows,
                        const std::vector<std::pair<std::wstring, std::wstring>>& extra_environment,
                        const std::wstring& working_directory) {
    if (impl_->running) {
        return false;
    }
    // A child that exited on its own leaves reader_thread joinable,
    // which would std::terminate() on reassignment below without this.
    Stop();

    HANDLE pty_input_read = nullptr;
    HANDLE pty_output_write = nullptr;
    if (!::CreatePipe(&pty_input_read, &impl_->input_write, nullptr, 0)) {
        impl_->last_error = "CreatePipe (input) failed";
        return false;
    }
    if (!::CreatePipe(&impl_->output_read, &pty_output_write, nullptr, 0)) {
        impl_->last_error = "CreatePipe (output) failed";
        ::CloseHandle(pty_input_read);
        impl_->CleanupHandles();
        return false;
    }

    const COORD size{cols, rows};
    const HRESULT hr = ::CreatePseudoConsole(size, pty_input_read, pty_output_write, 0, &impl_->pseudo_console);
    // ConPTY duplicates what it needs; the pipe ends passed in are ours to
    // close once it's created (see Microsoft's ConPTY sample).
    ::CloseHandle(pty_input_read);
    ::CloseHandle(pty_output_write);
    if (FAILED(hr)) {
        impl_->last_error = "CreatePseudoConsole failed";
        impl_->CleanupHandles();
        return false;
    }

    size_t attr_list_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_list_size);
    impl_->attribute_list =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(::HeapAlloc(::GetProcessHeap(), 0, attr_list_size));
    if (impl_->attribute_list == nullptr ||
        !::InitializeProcThreadAttributeList(impl_->attribute_list, 1, 0, &attr_list_size) ||
        !::UpdateProcThreadAttribute(impl_->attribute_list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                      impl_->pseudo_console, sizeof(HPCON), nullptr, nullptr)) {
        impl_->last_error = "Failed to build the pseudo console process attribute list";
        impl_->CleanupHandles();
        return false;
    }

    STARTUPINFOEXW startup_info{};
    startup_info.StartupInfo.cb = sizeof(startup_info);
    startup_info.lpAttributeList = impl_->attribute_list;
    // Without this, the child's console output was observed duplicating
    // into the parent's own stdout, in addition to arriving correctly via
    // the ConPTY pipe -- reproduced both launched from a shell and via
    // PowerShell's Start-Process, so it wasn't specific to either
    // launcher. bInheritHandles=FALSE alone did not stop it; leaving
    // STARTUPINFOEXW's std handles zeroed apparently left an implicit
    // inherit-from-parent path open despite that. Explicitly marking them
    // INVALID_HANDLE_VALUE closes it.
    startup_info.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
    startup_info.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
    startup_info.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
    startup_info.StartupInfo.hStdError = INVALID_HANDLE_VALUE;

    std::wstring mutable_cmd = command_line; // CreateProcessW requires a writable buffer
    std::vector<wchar_t> environment_block;
    LPVOID environment_ptr = nullptr;
    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT;
    if (!extra_environment.empty()) {
        environment_block = BuildEnvironmentBlock(extra_environment);
        environment_ptr = environment_block.data();
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
    }
    PROCESS_INFORMATION pi{};
    const wchar_t* working_directory_ptr = working_directory.empty() ? nullptr : working_directory.c_str();
    const BOOL ok = ::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE, creation_flags,
                                      environment_ptr, working_directory_ptr, &startup_info.StartupInfo, &pi);
    if (!ok) {
        impl_->last_error = "CreateProcessW failed (command not found?)";
        impl_->CleanupHandles();
        return false;
    }
    impl_->process_info = pi;
    AISTUDIO_LOG_INFO("Core.Terminal", "PtyProcess started, pid=" + std::to_string(pi.dwProcessId));

    {
        std::lock_guard lock(impl_->output_mutex);
        impl_->pending_output.clear();
    }
    impl_->last_error.clear();
    impl_->exit_code.reset();
    impl_->running = true;
    impl_->reader_thread = std::thread(&Impl::ReaderThreadMain, impl_.get());
    return true;
}

void PtyProcess::Stop() {
    if (impl_->process_info.hProcess != nullptr) {
        // Only overwrite exit_code if we're the ones ending a still-live
        // process here -- if IsRunning() already observed an organic exit
        // (impl_->running already false), its real exit code must survive,
        // not get replaced by the 0 TerminateProcess forces below.
        if (impl_->running) {
            impl_->exit_code = 0;
        }
        ::TerminateProcess(impl_->process_info.hProcess, 0);
        ::CloseHandle(impl_->process_info.hProcess);
        ::CloseHandle(impl_->process_info.hThread);
        impl_->process_info = {};
    }
    if (impl_->pseudo_console != nullptr) {
        ::ClosePseudoConsole(impl_->pseudo_console);
        impl_->pseudo_console = nullptr;
    }
    if (impl_->output_read != nullptr) {
        // Unblocks the reader thread's pending ReadFile() so it can exit.
        ::CloseHandle(impl_->output_read);
        impl_->output_read = nullptr;
    }
    if (impl_->reader_thread.joinable()) {
        impl_->reader_thread.join();
    }
    impl_->CleanupHandles();
    impl_->running = false;
}

void PtyProcess::Resize(std::int16_t cols, std::int16_t rows) {
    if (impl_->pseudo_console == nullptr) {
        return;
    }
    const COORD size{cols, rows};
    ::ResizePseudoConsole(impl_->pseudo_console, size);
}

bool PtyProcess::IsRunning() {
    if (!impl_->running || impl_->process_info.hProcess == nullptr) {
        return impl_->running;
    }
    DWORD exit_code = 0;
    if (::GetExitCodeProcess(impl_->process_info.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
        impl_->running = false;
        impl_->exit_code = static_cast<int>(exit_code);
        AISTUDIO_LOG_INFO("Core.Terminal", "PtyProcess child exited, code=" + std::to_string(exit_code));
    }
    return impl_->running;
}

std::optional<int> PtyProcess::ExitCode() const { return impl_->exit_code; }

const std::string& PtyProcess::LastErrorMessage() const {
    return impl_->last_error;
}

void PtyProcess::WriteInput(std::string_view bytes) {
    if (impl_->input_write == nullptr || bytes.empty()) {
        return;
    }
    DWORD written = 0;
    ::WriteFile(impl_->input_write, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
}

std::string PtyProcess::TakeOutput() {
    std::lock_guard lock(impl_->output_mutex);
    std::string out;
    out.swap(impl_->pending_output);
    return out;
}

#else

struct PtyProcess::Impl {};

PtyProcess::PtyProcess() : impl_(std::make_unique<Impl>()) {}
PtyProcess::~PtyProcess() = default;

bool PtyProcess::Start(const std::wstring&, std::int16_t, std::int16_t,
                        const std::vector<std::pair<std::wstring, std::wstring>>&, const std::wstring&) {
    return false;
}
void PtyProcess::Stop() {}
void PtyProcess::Resize(std::int16_t, std::int16_t) {}
bool PtyProcess::IsRunning() { return false; }
std::optional<int> PtyProcess::ExitCode() const { return std::nullopt; }
const std::string& PtyProcess::LastErrorMessage() const {
    static const std::string kMessage = "PtyProcess is not implemented on this platform";
    return kMessage;
}
void PtyProcess::WriteInput(std::string_view) {}
std::string PtyProcess::TakeOutput() { return {}; }

#endif

} // namespace aistudio::core
