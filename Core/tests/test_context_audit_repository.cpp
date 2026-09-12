#include "test_framework.hpp"
#include "Core/Database/ContextAuditRepository.hpp"

using namespace aistudio::core;

namespace {
ContextAuditEntry MakeEntry(std::string id, std::string item_id, ContextAuditAction action) {
    ContextAuditEntry entry;
    entry.id = std::move(id);
    entry.item_id = std::move(item_id);
    entry.action = action;
    entry.timestamp = 1700000000;
    entry.detail = "tokens=100";
    return entry;
}
} // namespace

AISTUDIO_TEST(ContextAuditRepository_Save_And_FindAll_RoundTrips) {
    Database db;
    db.Open(":memory:");
    ContextAuditRepository repo(db);
    repo.EnsureSchema();

    AISTUDIO_EXPECT(repo.Save(MakeEntry("audit-1", "a.cpp", ContextAuditAction::Included)));
    AISTUDIO_EXPECT(repo.Save(MakeEntry("audit-2", "b.cpp", ContextAuditAction::Excluded)));

    const auto result = repo.FindAll();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().size() == 2);
    AISTUDIO_EXPECT(result.Value()[0].item_id == "a.cpp");
    AISTUDIO_EXPECT(result.Value()[0].action == ContextAuditAction::Included);
    AISTUDIO_EXPECT(result.Value()[1].action == ContextAuditAction::Excluded);
}

AISTUDIO_TEST(ContextAuditRepository_FindByItemId_FiltersToOneItem) {
    Database db;
    db.Open(":memory:");
    ContextAuditRepository repo(db);
    repo.EnsureSchema();

    repo.Save(MakeEntry("audit-1", "a.cpp", ContextAuditAction::Included));
    repo.Save(MakeEntry("audit-2", "b.cpp", ContextAuditAction::Included));
    repo.Save(MakeEntry("audit-3", "a.cpp", ContextAuditAction::Compressed));

    const auto result = repo.FindByItemId("a.cpp");
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().size() == 2);
}

AISTUDIO_TEST(ContextAuditRepository_FindAll_EmptyWhenNoEntries) {
    Database db;
    db.Open(":memory:");
    ContextAuditRepository repo(db);
    repo.EnsureSchema();

    const auto result = repo.FindAll();
    AISTUDIO_EXPECT(result.IsOk());
    AISTUDIO_EXPECT(result.Value().empty());
}

AISTUDIO_TEST(ContextAuditRepository_PreservesDetailAndTimestamp) {
    Database db;
    db.Open(":memory:");
    ContextAuditRepository repo(db);
    repo.EnsureSchema();

    repo.Save(MakeEntry("audit-1", "a.cpp", ContextAuditAction::Included));

    const auto result = repo.FindAll();
    AISTUDIO_EXPECT(result.Value()[0].detail == "tokens=100");
    AISTUDIO_EXPECT(result.Value()[0].timestamp == 1700000000);
}
