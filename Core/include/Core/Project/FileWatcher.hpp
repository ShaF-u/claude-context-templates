#pragma once

#include "Core/Project/FileScanner.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

enum class FileChangeKind { Created, Modified, Deleted };

struct FileChangeEvent {
    FileChangeKind kind;
    std::string path; // relative to the watched root
};

// Change detector: re-scans the project root via FileScanner and diffs
// content_hash against the previous snapshot, publishing each
// FileChangeEvent to EventBus under "FileChanged" (AGENT.md #5).
// PollOnce() itself is the whole diffing algorithm and is platform-
// independent; Start()'s background loop drives when it runs. On Windows
// (see FileWatcher.cpp), that loop uses ReadDirectoryChangesW to wait for
// a real filesystem notification instead of sleeping for the full
// `poll_interval`, so a change is normally picked up within a couple
// hundred milliseconds rather than up to `poll_interval` later;
// `poll_interval` still bounds the wait as a fallback (also used as the
// plain timer interval on non-Windows, or if the native watch can't be
// set up, e.g. a root that doesn't exist yet).
//
// The very first successful PollOnce() call has no prior snapshot to diff
// against, so every file the scan finds would otherwise be reported as
// Created -- not because it just appeared, but purely because this is the
// first time the watcher has looked. That first call is therefore treated
// as a silent baseline: known_hashes_ is populated but no FileChangeEvent
// is emitted for any of it. Every consumer (Core/src/main.cpp's HTTP mode
// and RunMcpMode()) already builds SymbolIndex/IncludeGraph/CallGraph/
// InheritanceGraph/ReferenceGraph/AstIndex from an explicit FileScanner-
// based Build() call before constructing/Start()-ing a FileWatcher, so
// this first-poll burst was always redundant re-indexing, not real initial
// population -- silencing it is safe and lets Start()'s Windows loop arm
// ReadDirectoryChangesW right away instead of blocking on a synchronous
// full-project reindex of everyone's "FileChanged" subscribers first (see
// docs/ROADMAP.md's "FileWatcher startup burst" narrative for how this was
// found). A later genuinely-new file still reports Created normally, since
// only the very first successful poll is treated as baseline.
class FileWatcher {
public:
    FileWatcher(std::string root, FileScanner scanner,
                std::chrono::milliseconds poll_interval = std::chrono::milliseconds(1000));
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    void Start();
    void Stop();

    // Re-scans immediately and returns the diff against the last known
    // snapshot, without touching the background thread. Used by Start()'s
    // loop, and directly by tests / a manual "Reload" action.
    std::vector<FileChangeEvent> PollOnce();

private:
    std::string root_;
    FileScanner scanner_;
    std::chrono::milliseconds poll_interval_;
    std::unordered_map<std::string, std::string> known_hashes_; // path -> content_hash
    // Set once the first successful PollOnce() has populated known_hashes_
    // as a silent baseline (see the class comment above). Only PollOnce()
    // itself (single-threaded per call, called from either the owning
    // thread directly or serially from Start()'s background loop) reads or
    // writes this, so it doesn't need to be atomic like running_.
    bool baseline_established_ = false;
    std::atomic<bool> running_{false};
    std::thread thread_;

    // Windows-only: a manual-reset event Stop() signals to wake a
    // ReadDirectoryChangesW wait immediately, instead of waiting out the
    // rest of poll_interval_. Kept as void* (a HANDLE) so this header
    // doesn't need <windows.h> -- unused on non-Windows builds (matches
    // PluginLoader.hpp's same void*-handle convention, since this header
    // is pulled in transitively by IncludeGraph.hpp/SymbolIndex.hpp and
    // beyond). std::atomic because Start()'s background thread and
    // whichever thread calls Stop() both touch it.
    std::atomic<void*> stop_event_{nullptr};
};

} // namespace aistudio::core
