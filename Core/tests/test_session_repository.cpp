#include "test_framework.hpp"
#include "Core/Database/SessionRepository.hpp"

using namespace aistudio::core;

AISTUDIO_TEST(SessionRepository_SaveAndFindById_RoundTrips) {
    Database db;
    db.Open(":memory:");
    SessionRepository repo(db);
    repo.EnsureSchema();

    Session session;
    session.id = "s1";
    session.project_id = "proj1";
    session.workspace_id = "ws1";
    session.task_id = "t1";
    session.cli_name = "claude";
    session.state = SessionState::Running;
    session.created_at = 100;
    session.started_at = 101;
    session.ended_at = 0;

    AISTUDIO_EXPECT(repo.Save(session));

    const auto found_result = repo.FindById("s1");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(found_result.Value().has_value());

    const auto& loaded = *found_result.Value();
    AISTUDIO_EXPECT(loaded.project_id == "proj1");
    AISTUDIO_EXPECT(loaded.workspace_id == "ws1");
    AISTUDIO_EXPECT(loaded.task_id == "t1");
    AISTUDIO_EXPECT(loaded.cli_name == "claude");
    AISTUDIO_EXPECT(loaded.state == SessionState::Running);
    AISTUDIO_EXPECT(loaded.created_at == 100);
    AISTUDIO_EXPECT(loaded.started_at == 101);
    AISTUDIO_EXPECT(loaded.ended_at == 0);
}

AISTUDIO_TEST(SessionRepository_FindById_MissingId_ReturnsNullopt) {
    Database db;
    db.Open(":memory:");
    SessionRepository repo(db);
    repo.EnsureSchema();

    const auto found_result = repo.FindById("does_not_exist");
    AISTUDIO_EXPECT(found_result.IsOk());
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}

AISTUDIO_TEST(SessionRepository_Save_UpsertsExistingId) {
    Database db;
    db.Open(":memory:");
    SessionRepository repo(db);
    repo.EnsureSchema();

    Session session;
    session.id = "s1";
    session.state = SessionState::Running;
    session.created_at = 100;
    AISTUDIO_EXPECT(repo.Save(session));

    session.state = SessionState::Terminated;
    session.ended_at = 200;
    AISTUDIO_EXPECT(repo.Save(session));

    const auto found_result = repo.FindById("s1");
    AISTUDIO_EXPECT(found_result.Value().has_value());
    AISTUDIO_EXPECT(found_result.Value()->state == SessionState::Terminated);
    AISTUDIO_EXPECT(found_result.Value()->ended_at == 200);
}

AISTUDIO_TEST(SessionRepository_FindAll_ReturnsInCreatedOrder) {
    Database db;
    db.Open(":memory:");
    SessionRepository repo(db);
    repo.EnsureSchema();

    Session first;
    first.id = "s1";
    first.state = SessionState::Terminated;
    first.created_at = 100;
    AISTUDIO_EXPECT(repo.Save(first));

    Session second;
    second.id = "s2";
    second.state = SessionState::Running;
    second.created_at = 200;
    AISTUDIO_EXPECT(repo.Save(second));

    const auto all_result = repo.FindAll();
    AISTUDIO_EXPECT(all_result.IsOk());
    AISTUDIO_EXPECT(all_result.Value().size() == 2);
    AISTUDIO_EXPECT(all_result.Value()[0].id == "s1");
    AISTUDIO_EXPECT(all_result.Value()[1].id == "s2");
}

AISTUDIO_TEST(SessionRepository_Remove_DeletesRow) {
    Database db;
    db.Open(":memory:");
    SessionRepository repo(db);
    repo.EnsureSchema();

    Session session;
    session.id = "s1";
    session.state = SessionState::Running;
    AISTUDIO_EXPECT(repo.Save(session));
    AISTUDIO_EXPECT(repo.Remove("s1"));

    const auto found_result = repo.FindById("s1");
    AISTUDIO_EXPECT(!found_result.Value().has_value());
}
