#include "test_framework.hpp"
#include "Core/Database/TaskRepository.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(TaskRepository_SaveAndFindById_RoundTrips) {
    Database db;
    db.Open(":memory:");
    TaskRepository repo(db);
    repo.EnsureSchema();

    Task task;
    task.id = "t1";
    task.name = "Fix player's health"; // deliberately contains an apostrophe
    task.state = TaskState::Running;
    task.progress = 0.4;
    task.retry_count = 1;
    task.max_retries = 3;
    task.depends_on = {"t0"};

    AISTUDIO_EXPECT(repo.Save(task));

    const auto found_result = repo.FindById("t1");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.name == "Fix player's health");
    AISTUDIO_EXPECT(loaded.state == TaskState::Running);
    AISTUDIO_EXPECT(loaded.progress == 0.4);
    AISTUDIO_EXPECT(loaded.depends_on.size() == 1);
    AISTUDIO_EXPECT(loaded.depends_on[0] == "t0");
}

AISTUDIO_TEST(TaskRepository_Save_UpsertsExistingId) {
    Database db;
    db.Open(":memory:");
    TaskRepository repo(db);
    repo.EnsureSchema();

    Task task;
    task.id = "t1";
    task.name = "first";
    repo.Save(task);

    task.name = "second";
    repo.Save(task);

    const auto all_result = repo.FindAll();
    AISTUDIO_EXPECT(all_result.IsOk());
    AISTUDIO_EXPECT(all_result.Value().size() == 1);
    AISTUDIO_EXPECT(all_result.Value()[0].name == "second");
}

AISTUDIO_TEST(TaskRepository_FindById_MissingReturnsNullopt) {
    Database db;
    db.Open(":memory:");
    TaskRepository repo(db);
    repo.EnsureSchema();

    const auto result = repo.FindById("missing");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(!result.Value().has_value());
}

AISTUDIO_TEST(TaskRepository_Remove_DeletesRow) {
    Database db;
    db.Open(":memory:");
    TaskRepository repo(db);
    repo.EnsureSchema();

    Task task;
    task.id = "t1";
    task.name = "x";
    repo.Save(task);
    AISTUDIO_EXPECT(repo.Remove("t1"));

    const auto result = repo.FindById("t1");
    AISTUDIO_EXPECT(!result.Value().has_value());
}

AISTUDIO_TEST(TaskRepository_FindAll_EmptyWhenNoTasks) {
    Database db;
    db.Open(":memory:");
    TaskRepository repo(db);
    repo.EnsureSchema();

    const auto result = repo.FindAll();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().empty());
}
