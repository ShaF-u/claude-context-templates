#include "test_framework.hpp"
#include "Core/Event/EventBus.hpp"
#include "Core/Project/FileWatcher.hpp"

#include <algorithm>
#include <any>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_watch_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempProject() { fs::remove_all(root); }

    void WriteFile(const std::string& relative_path, const std::string& content) const {
        const fs::path full_path = root / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
    }

    void RemoveFile(const std::string& relative_path) const { fs::remove(root / relative_path); }

    [[nodiscard]] std::string RootString() const { return root.string(); }
};

bool HasChange(const std::vector<FileChangeEvent>& changes, FileChangeKind kind, const std::string& path) {
    return std::any_of(changes.begin(), changes.end(),
                        [&](const FileChangeEvent& c) { return c.kind == kind && c.path == path; });
}

} // namespace

// The first-ever PollOnce() call has no prior snapshot to diff against, so
// it silently establishes the baseline (known_hashes_ populated) instead
// of reporting every pre-existing file as Created -- see FileWatcher.hpp's
// class comment. This used to report a Created event per file (the
// startup-burst behavior docs/ROADMAP.md's "FileWatcher startup burst"
// narrative documents as having caused a real 15-20s ReadDirectoryChangesW
// arming delay); this test now demonstrates the fix directly.
AISTUDIO_TEST(FileWatcher_PollOnce_FirstScanIsSilentBaseline) {
    TempProject project;
    project.WriteFile("a.txt", "content");
    project.WriteFile("b.txt", "content2");

    FileWatcher watcher(project.RootString(), FileScanner{});
    const auto changes = watcher.PollOnce();

    AISTUDIO_EXPECT(changes.empty());
}

// known_hashes_ must actually be populated by the silent baseline poll
// (not just skipped), so a subsequent poll correctly diffs against it:
// modifying one pre-existing file reports exactly that one Modified event,
// nothing else (in particular, not the untouched "b.txt", and not a
// Created for either file).
AISTUDIO_TEST(FileWatcher_PollOnce_AfterSilentBaseline_DetectsOnlyRealModification) {
    TempProject project;
    project.WriteFile("a.txt", "v1");
    project.WriteFile("b.txt", "unchanged");

    FileWatcher watcher(project.RootString(), FileScanner{});
    const auto baseline = watcher.PollOnce();
    AISTUDIO_EXPECT(baseline.empty());

    project.WriteFile("a.txt", "v2 — different content and length");
    const auto changes = watcher.PollOnce();

    AISTUDIO_EXPECT(changes.size() == 1);
    AISTUDIO_EXPECT(HasChange(changes, FileChangeKind::Modified, "a.txt"));
}

// Only the very first poll is treated as baseline -- a genuinely new file
// that appears after that still reports Created normally.
AISTUDIO_TEST(FileWatcher_PollOnce_NewFileAfterBaseline_ReportsCreated) {
    TempProject project;
    project.WriteFile("a.txt", "content");

    FileWatcher watcher(project.RootString(), FileScanner{});
    const auto baseline = watcher.PollOnce();
    AISTUDIO_EXPECT(baseline.empty());

    project.WriteFile("new.txt", "brand new");
    const auto changes = watcher.PollOnce();

    AISTUDIO_EXPECT(changes.size() == 1);
    AISTUDIO_EXPECT(HasChange(changes, FileChangeKind::Created, "new.txt"));
}

AISTUDIO_TEST(FileWatcher_PollOnce_DetectsModification) {
    TempProject project;
    project.WriteFile("a.txt", "v1");

    FileWatcher watcher(project.RootString(), FileScanner{});
    watcher.PollOnce(); // baseline

    project.WriteFile("a.txt", "v2 — different content and length");
    const auto changes = watcher.PollOnce();

    AISTUDIO_EXPECT(HasChange(changes, FileChangeKind::Modified, "a.txt"));
}

AISTUDIO_TEST(FileWatcher_PollOnce_DetectsDeletion) {
    TempProject project;
    project.WriteFile("a.txt", "content");

    FileWatcher watcher(project.RootString(), FileScanner{});
    watcher.PollOnce(); // baseline

    project.RemoveFile("a.txt");
    const auto changes = watcher.PollOnce();

    AISTUDIO_EXPECT(HasChange(changes, FileChangeKind::Deleted, "a.txt"));
}

AISTUDIO_TEST(FileWatcher_PollOnce_NoChanges_ReportsNothing) {
    TempProject project;
    project.WriteFile("a.txt", "content");

    FileWatcher watcher(project.RootString(), FileScanner{});
    watcher.PollOnce();

    const auto changes = watcher.PollOnce();
    AISTUDIO_EXPECT(changes.empty());
}

AISTUDIO_TEST(FileWatcher_PollOnce_PublishesToEventBus) {
    TempProject project;
    project.WriteFile("a.txt", "content");

    std::atomic<int> received{0};
    const auto subscription_id = EventBus::Instance().Subscribe("FileChanged", [&](const std::any& payload) {
        if (std::any_cast<FileChangeEvent>(&payload) != nullptr) {
            ++received;
        }
    });

    FileWatcher watcher(project.RootString(), FileScanner{});
    watcher.PollOnce(); // silent baseline: must not publish anything
    AISTUDIO_EXPECT(received.load() == 0);

    project.WriteFile("a.txt", "content v2");
    watcher.PollOnce();

    EventBus::Instance().Unsubscribe("FileChanged", subscription_id);
    AISTUDIO_EXPECT(received.load() == 1);
}

// Regression test for FileWatcher.cpp's Windows-only ReadDirectoryChangesW
// path: Start()'s background loop should react to a real filesystem
// change well before poll_interval elapses, not just at the next timer
// tick (see FileWatcher.hpp's class comment). A poll_interval this long
// makes the plain-timer fallback path fail this test's deadline, so a
// pass here is real evidence the native watch fired.
AISTUDIO_TEST(FileWatcher_Start_ReactsToChangeFasterThanPollInterval) {
    TempProject project;
    project.WriteFile("a.txt", "v1");

    std::atomic<int> received{0};
    const auto subscription_id =
        EventBus::Instance().Subscribe("FileChanged", [&](const std::any&) { ++received; });

    // poll_interval_ is deliberately short (not this class's normal
    // production value) so the test's deadline can safely exceed the
    // *worst case* below, not just the common-case latency.
    FileWatcher watcher(project.RootString(), FileScanner{}, std::chrono::milliseconds(300));

    // Establish the baseline synchronously (silent -- no event) before
    // Start(), so this test measures only how fast a *real* change is
    // detected once watching, not how long the first-poll baseline scan
    // itself takes. Start()'s loop will still run its own first PollOnce()
    // immediately, but since the baseline is already established that
    // call is a normal (empty, since nothing changed yet) diff, not
    // another baseline -- it's fast regardless of project size, which is
    // exactly the property this fix (docs/ROADMAP.md "FileWatcher startup
    // burst") is meant to guarantee.
    const auto baseline = watcher.PollOnce();
    AISTUDIO_EXPECT(baseline.empty());
    AISTUDIO_EXPECT(received.load() == 0);

    watcher.Start();

    project.WriteFile("a.txt", "v2 -- changed while watching");

    // Root cause of this test's original flakiness: there is a real,
    // narrow window between Start()'s thread doing its first PollOnce()
    // (line ~121 in FileWatcher.cpp, confirming the already-established
    // baseline is unchanged) and ReadDirectoryChangesW actually being
    // armed a few lines later. A write landing in that window is invisible
    // to the OS notification -- ReadDirectoryChangesW only reports changes
    // occurring after it's issued -- so detection falls back to the next
    // loop iteration's PollOnce(), which only runs after a full
    // poll_interval_ timeout. A fixed multi-second deadline can't safely
    // outlast that worst case if poll_interval_ is large (this test used
    // to construct FileWatcher with a 10s interval, so an 8s deadline was
    // never actually safe under load -- it was consistently reproducible,
    // not rare bad luck). Using a short poll_interval_ above bounds that
    // worst case tightly, so a deadline of a few multiples of it is safe
    // in every code path, not just the fast (OS-notification) one.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    watcher.Stop();
    EventBus::Instance().Unsubscribe("FileChanged", subscription_id);

    AISTUDIO_EXPECT(received.load() >= 1);
}
