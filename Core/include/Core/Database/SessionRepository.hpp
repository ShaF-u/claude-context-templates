#pragma once

#include "Core/Database/Database.hpp"
#include "Core/Error/Result.hpp"
#include "Core/Session/Session.hpp"

#include <optional>
#include <vector>

namespace aistudio::core {

// Persists Session to SQLite (docs/ROADMAP.md 13-2's "セッション情報の
// 永続化" -- so session history survives an app restart, matching
// TaskRepository's own shape/reasoning exactly). Holds no adapter/process
// handle -- those live only in SessionManager, in-memory, for the
// lifetime of the process that created them.
class SessionRepository {
public:
    explicit SessionRepository(Database& database) : database_(database) {}

    Result<void> EnsureSchema();
    Result<void> Save(const Session& session);
    [[nodiscard]] Result<std::optional<Session>> FindById(const std::string& id);
    [[nodiscard]] Result<std::vector<Session>> FindAll();
    Result<void> Remove(const std::string& id);

private:
    Database& database_;
};

} // namespace aistudio::core
