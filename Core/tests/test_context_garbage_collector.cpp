#include "test_framework.hpp"
#include "Core/Context/ContextGarbageCollector.hpp"

using namespace aistudio::core;

namespace {

ContextSnapshot MakeSnapshot(std::string id, std::int64_t created_at) {
    ContextSnapshot snapshot;
    snapshot.id = std::move(id);
    snapshot.task_name = "test";
    snapshot.created_at = created_at;
    return snapshot;
}

} // namespace

AISTUDIO_TEST(ContextGarbageCollector_CollectGarbage_WithinLimit_RemovesNone) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();
    repo.Save(MakeSnapshot("snap-1", 100));
    repo.Save(MakeSnapshot("snap-2", 200));

    const ContextGarbageCollector gc(repo, ContextGarbageCollector::Options{.max_snapshots_to_keep = 5});
    const auto result = gc.CollectGarbage();

    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value() == 0);
    AISTUDIO_EXPECT(repo.FindAll().Value().size() == 2);
}

AISTUDIO_TEST(ContextGarbageCollector_CollectGarbage_OverLimit_RemovesOldest) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();
    repo.Save(MakeSnapshot("oldest", 100));
    repo.Save(MakeSnapshot("middle", 200));
    repo.Save(MakeSnapshot("newest", 300));

    const ContextGarbageCollector gc(repo, ContextGarbageCollector::Options{.max_snapshots_to_keep = 2});
    const auto result = gc.CollectGarbage();

    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value() == 1);

    const auto remaining = repo.FindAll().Value();
    AISTUDIO_EXPECT(remaining.size() == 2);
    AISTUDIO_EXPECT(!repo.FindById("oldest").Value().has_value());
    AISTUDIO_EXPECT(repo.FindById("middle").Value().has_value());
    AISTUDIO_EXPECT(repo.FindById("newest").Value().has_value());
}

AISTUDIO_TEST(ContextGarbageCollector_CollectGarbage_ExactlyAtLimit_RemovesNone) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();
    repo.Save(MakeSnapshot("snap-1", 100));
    repo.Save(MakeSnapshot("snap-2", 200));

    const ContextGarbageCollector gc(repo, ContextGarbageCollector::Options{.max_snapshots_to_keep = 2});
    const auto result = gc.CollectGarbage();

    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value() == 0);
    AISTUDIO_EXPECT(repo.FindAll().Value().size() == 2);
}

AISTUDIO_TEST(ContextGarbageCollector_CollectGarbage_EmptyRepository_RemovesNone) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();

    const ContextGarbageCollector gc(repo);
    const auto result = gc.CollectGarbage();

    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value() == 0);
}

AISTUDIO_TEST(ContextGarbageCollector_CollectGarbage_DefaultOptions_Keeps100) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();
    for (int i = 0; i < 5; ++i) {
        repo.Save(MakeSnapshot("snap-" + std::to_string(i), 100 + i));
    }

    const ContextGarbageCollector gc(repo); // default max_snapshots_to_keep = 100
    const auto result = gc.CollectGarbage();

    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value() == 0);
    AISTUDIO_EXPECT(repo.FindAll().Value().size() == 5);
}

AISTUDIO_TEST(ContextGarbageCollector_CollectGarbage_ZeroToKeep_RemovesAll) {
    Database db;
    db.Open(":memory:");
    ContextSnapshotRepository repo(db);
    repo.EnsureSchema();
    repo.Save(MakeSnapshot("snap-1", 100));
    repo.Save(MakeSnapshot("snap-2", 200));

    const ContextGarbageCollector gc(repo, ContextGarbageCollector::Options{.max_snapshots_to_keep = 0});
    const auto result = gc.CollectGarbage();

    AISTUDIO_EXPECT(result);
    AISTUDIO_EXPECT(result.Value() == 2);
    AISTUDIO_EXPECT(repo.FindAll().Value().empty());
}
