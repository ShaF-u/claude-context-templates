#include "Core/Project/FileWatcher.hpp"

#include "Core/Event/EventBus.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace aistudio::core {

FileWatcher::FileWatcher(std::string root, FileScanner scanner, std::chrono::milliseconds poll_interval)
    : root_(std::move(root)), scanner_(std::move(scanner)), poll_interval_(poll_interval) {}

FileWatcher::~FileWatcher() {
    Stop();
}

std::vector<FileChangeEvent> FileWatcher::PollOnce() {
    std::vector<FileChangeEvent> changes;

    const auto scan_result = scanner_.Scan(root_);
    if (!scan_result) {
        return changes;
    }

    std::unordered_map<std::string, std::string> current_hashes;
    for (const auto& metadata : scan_result.Value()) {
        current_hashes.emplace(metadata.path, metadata.content_hash);
    }

    // First successful poll: silently establish the baseline instead of
    // diffing against an empty known_hashes_ (which would otherwise report
    // every pre-existing file as Created -- see FileWatcher.hpp's class
    // comment for why that's both wrong and expensive).
    if (!baseline_established_) {
        known_hashes_ = std::move(current_hashes);
        baseline_established_ = true;
        return changes;
    }

    for (const auto& [path, hash] : current_hashes) {
        const auto it = known_hashes_.find(path);
        if (it == known_hashes_.end()) {
            changes.push_back(FileChangeEvent{FileChangeKind::Created, path});
        } else if (it->second != hash) {
            changes.push_back(FileChangeEvent{FileChangeKind::Modified, path});
        }
    }
    for (const auto& [path, hash] : known_hashes_) {
        if (!current_hashes.contains(path)) {
            changes.push_back(FileChangeEvent{FileChangeKind::Deleted, path});
        }
    }

    known_hashes_ = std::move(current_hashes);

    for (const auto& change : changes) {
        EventBus::Instance().Publish("FileChanged", change);
    }

    return changes;
}

#if defined(_WIN32)

namespace {

std::wstring WidenUtf8(const std::string& utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }
    const int wide_length = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(wide_length), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wide_length);
    return wide;
}

// How long to wait after a real change notification before actually
// calling PollOnce() again, so a burst of individual file writes (e.g. a
// git checkout touching hundreds of files, or an editor's save-as
// temp-file-then-rename dance) collapses into one rescan instead of one
// per notification.
constexpr auto kDebounceWindow = std::chrono::milliseconds(200);

} // namespace

void FileWatcher::Start() {
    if (running_.exchange(true)) {
        return;
    }

    stop_event_.store(::CreateEventW(nullptr, /*bManualReset=*/TRUE, /*bInitialState=*/FALSE, nullptr));

    thread_ = std::thread([this] {
        const HANDLE stop_event = static_cast<HANDLE>(stop_event_.load());
        const HANDLE directory_handle =
            ::CreateFileW(WidenUtf8(root_).c_str(), FILE_LIST_DIRECTORY,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory_handle == INVALID_HANDLE_VALUE) {
            // Falls back to plain timer-based polling -- e.g. root_
            // doesn't exist yet, or this process lacks permission to open
            // it for change notification.
            while (running_.load()) {
                PollOnce();
                if (::WaitForSingleObject(stop_event, static_cast<DWORD>(poll_interval_.count())) == WAIT_OBJECT_0) {
                    break;
                }
            }
            ::CloseHandle(stop_event);
            stop_event_.store(nullptr);
            return;
        }

        std::vector<BYTE> notify_buffer(4096);
        OVERLAPPED overlapped{};
        overlapped.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);

        while (running_.load()) {
            PollOnce();

            ::ResetEvent(overlapped.hEvent);
            DWORD bytes_returned = 0;
            const BOOL issued = ::ReadDirectoryChangesW(
                directory_handle, notify_buffer.data(), static_cast<DWORD>(notify_buffer.size()),
                /*bWatchSubtree=*/TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_SIZE,
                &bytes_returned, &overlapped, nullptr);
            if (!issued) {
                // Same fallback rationale as the CreateFileW failure above
                // -- couldn't queue the watch this time, so just wait out
                // one plain interval and try again next iteration.
                if (::WaitForSingleObject(stop_event, static_cast<DWORD>(poll_interval_.count())) == WAIT_OBJECT_0) {
                    break;
                }
                continue;
            }

            const HANDLE wait_handles[2] = {overlapped.hEvent, stop_event};
            const DWORD wait_result =
                ::WaitForMultipleObjects(2, wait_handles, FALSE, static_cast<DWORD>(poll_interval_.count()));

            if (wait_result == WAIT_OBJECT_0 + 1) {
                // Stop() was called: the pending read is still outstanding
                // against notify_buffer/overlapped, so cancel and reap it
                // before those go out of scope.
                ::CancelIoEx(directory_handle, &overlapped);
                DWORD unused = 0;
                ::GetOverlappedResult(directory_handle, &overlapped, &unused, TRUE);
                break;
            }
            if (wait_result == WAIT_OBJECT_0) {
                // A real notification arrived; reap it (required before
                // the OVERLAPPED can be reused) and let a short burst of
                // further writes settle before re-scanning.
                DWORD unused = 0;
                ::GetOverlappedResult(directory_handle, &overlapped, &unused, FALSE);
                std::this_thread::sleep_for(kDebounceWindow);
            } else {
                // WAIT_TIMEOUT: poll_interval_ elapsed with nothing
                // reported. The read is still pending against
                // notify_buffer/overlapped -- cancel and reap it so the
                // next loop iteration can safely reuse both.
                ::CancelIoEx(directory_handle, &overlapped);
                DWORD unused = 0;
                ::GetOverlappedResult(directory_handle, &overlapped, &unused, TRUE);
            }
        }

        ::CloseHandle(overlapped.hEvent);
        ::CloseHandle(directory_handle);
        ::CloseHandle(stop_event);
        stop_event_.store(nullptr);
    });
}

void FileWatcher::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (const HANDLE stop_event = static_cast<HANDLE>(stop_event_.load()); stop_event != nullptr) {
        ::SetEvent(stop_event);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

#else

void FileWatcher::Start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] {
        while (running_.load()) {
            PollOnce();
            std::this_thread::sleep_for(poll_interval_);
        }
    });
}

void FileWatcher::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

#endif

} // namespace aistudio::core
