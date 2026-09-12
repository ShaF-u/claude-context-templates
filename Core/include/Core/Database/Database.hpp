#pragma once

#include "Core/Error/Result.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

struct sqlite3;

namespace aistudio::core {

// monostate represents SQL NULL.
using SqlValue = std::variant<std::monostate, std::int64_t, double, std::string>;

// One result row: column name -> value, in column order.
using Row = std::vector<std::pair<std::string, SqlValue>>;

// Thin RAII wrapper around SQLite3's C API (vendored amalgamation, see
// docs/DEPENDENCY_MANAGEMENT.md). Every statement is parameter-bound via
// `?` placeholders + SqlValue — never string-concatenated — so callers
// cannot accidentally introduce SQL injection. Higher-level pieces
// (Migrator, per-entity Repositories) are built on top of this instead
// of linking sqlite3 directly (AGENT.md #2).
class Database {
public:
    Database() = default;
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;

    Result<void> Open(const std::string& path);
    void Close();
    [[nodiscard]] bool IsOpen() const { return handle_ != nullptr; }

    // DDL / INSERT / UPDATE / DELETE. `params` bind positionally to `?`.
    Result<void> Execute(const std::string& sql, const std::vector<SqlValue>& params = {});

    // SELECT. Returns every result row.
    [[nodiscard]] Result<std::vector<Row>> Query(const std::string& sql, const std::vector<SqlValue>& params = {});

    Result<void> Begin();
    Result<void> Commit();
    Result<void> Rollback();

    // Live-copies this database to `destination_path` via SQLite's Online
    // Backup API (docs/ROADMAP.md Phase 1 "Backup / recovery").
    Result<void> BackupTo(const std::string& destination_path);

private:
    sqlite3* handle_ = nullptr;
};

// Row/SqlValue accessors with the type-coercion and NULL handling a
// Repository needs when mapping a Row back to a domain object.
[[nodiscard]] std::string AsString(const SqlValue& value);
[[nodiscard]] std::int64_t AsInt64(const SqlValue& value);
[[nodiscard]] double AsDouble(const SqlValue& value);

} // namespace aistudio::core
