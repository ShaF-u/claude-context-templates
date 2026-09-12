#include "Core/Database/Database.hpp"

#include <sqlite3.h>

namespace aistudio::core {

namespace {

Result<void> BindParams(sqlite3_stmt* stmt, const std::vector<SqlValue>& params) {
    for (std::size_t i = 0; i < params.size(); ++i) {
        const int index = static_cast<int>(i) + 1;
        const auto& value = params[i];
        int rc = SQLITE_OK;
        if (std::holds_alternative<std::monostate>(value)) {
            rc = sqlite3_bind_null(stmt, index);
        } else if (std::holds_alternative<std::int64_t>(value)) {
            rc = sqlite3_bind_int64(stmt, index, std::get<std::int64_t>(value));
        } else if (std::holds_alternative<double>(value)) {
            rc = sqlite3_bind_double(stmt, index, std::get<double>(value));
        } else {
            const auto& text = std::get<std::string>(value);
            rc = sqlite3_bind_text(stmt, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
        }
        if (rc != SQLITE_OK) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::Internal,
                .message = "failed to bind parameter " + std::to_string(index),
                .module = "Core.Database",
            });
        }
    }
    return Result<void>::Ok();
}

SqlValue ColumnValue(sqlite3_stmt* stmt, int column) {
    switch (sqlite3_column_type(stmt, column)) {
        case SQLITE_INTEGER:
            return static_cast<std::int64_t>(sqlite3_column_int64(stmt, column));
        case SQLITE_FLOAT:
            return sqlite3_column_double(stmt, column);
        case SQLITE_NULL:
            return std::monostate{};
        default: {
            const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, column));
            return std::string(text != nullptr ? text : "");
        }
    }
}

} // namespace

Database::~Database() {
    Close();
}

Database::Database(Database&& other) noexcept : handle_(other.handle_) {
    other.handle_ = nullptr;
}

Database& Database::operator=(Database&& other) noexcept {
    if (this != &other) {
        Close();
        handle_ = other.handle_;
        other.handle_ = nullptr;
    }
    return *this;
}

Result<void> Database::Open(const std::string& path) {
    Close();
    sqlite3* handle = nullptr;
    const int rc = sqlite3_open(path.c_str(), &handle);
    if (rc != SQLITE_OK) {
        const std::string message = handle != nullptr ? sqlite3_errmsg(handle) : "unknown sqlite3_open failure";
        if (handle != nullptr) {
            sqlite3_close(handle);
        }
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to open database '" + path + "': " + message,
            .module = "Core.Database",
        });
    }
    handle_ = handle;
    sqlite3_exec(handle_, "PRAGMA foreign_keys = ON;", nullptr, nullptr, nullptr);
    // This codebase's architecture has multiple separate OS processes (the
    // GUI and a separately-launched aistudio_core_cli in HTTP mode) open
    // their own connection to the same on-disk database file concurrently.
    // Without a busy timeout, one connection briefly holding SQLite's
    // write lock (e.g. mid-ALTER TABLE during a schema migration) makes
    // any other connection's concurrent write fail immediately with
    // SQLITE_BUSY instead of simply waiting the (usually sub-millisecond)
    // moment for the lock to clear. 5s is generous enough to ride out any
    // realistic same-process-generation contention without masking a
    // truly stuck lock forever.
    sqlite3_busy_timeout(handle_, 5000);
    return Result<void>::Ok();
}

void Database::Close() {
    if (handle_ != nullptr) {
        sqlite3_close(handle_);
        handle_ = nullptr;
    }
}

Result<void> Database::Execute(const std::string& sql, const std::vector<SqlValue>& params) {
    if (handle_ == nullptr) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument, .message = "database is not open", .module = "Core.Database"});
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::ParseError,
            .message = "failed to prepare statement: " + std::string(sqlite3_errmsg(handle_)),
            .module = "Core.Database",
        });
    }
    if (const auto bind_result = BindParams(stmt, params); !bind_result) {
        sqlite3_finalize(stmt);
        return bind_result;
    }
    const int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "statement execution failed: " + std::string(sqlite3_errmsg(handle_)),
            .module = "Core.Database",
        });
    }
    return Result<void>::Ok();
}

Result<std::vector<Row>> Database::Query(const std::string& sql, const std::vector<SqlValue>& params) {
    if (handle_ == nullptr) {
        return Result<std::vector<Row>>::Fail(
            Error{.code = ErrorCode::InvalidArgument, .message = "database is not open", .module = "Core.Database"});
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(handle_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return Result<std::vector<Row>>::Fail(Error{
            .code = ErrorCode::ParseError,
            .message = "failed to prepare query: " + std::string(sqlite3_errmsg(handle_)),
            .module = "Core.Database",
        });
    }
    if (const auto bind_result = BindParams(stmt, params); !bind_result) {
        const auto error = bind_result.Err();
        sqlite3_finalize(stmt);
        return Result<std::vector<Row>>::Fail(error);
    }

    std::vector<Row> rows;
    int rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW) {
        Row row;
        const int column_count = sqlite3_column_count(stmt);
        row.reserve(static_cast<std::size_t>(column_count));
        for (int i = 0; i < column_count; ++i) {
            row.emplace_back(sqlite3_column_name(stmt, i), ColumnValue(stmt, i));
        }
        rows.push_back(std::move(row));
        rc = sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return Result<std::vector<Row>>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "query execution failed: " + std::string(sqlite3_errmsg(handle_)),
            .module = "Core.Database",
        });
    }
    return Result<std::vector<Row>>::Ok(std::move(rows));
}

Result<void> Database::Begin() { return Execute("BEGIN;"); }
Result<void> Database::Commit() { return Execute("COMMIT;"); }
Result<void> Database::Rollback() { return Execute("ROLLBACK;"); }

Result<void> Database::BackupTo(const std::string& destination_path) {
    if (handle_ == nullptr) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument, .message = "database is not open", .module = "Core.Database"});
    }

    sqlite3* dest = nullptr;
    if (sqlite3_open(destination_path.c_str(), &dest) != SQLITE_OK) {
        const std::string message = dest != nullptr ? sqlite3_errmsg(dest) : "unknown sqlite3_open failure";
        if (dest != nullptr) {
            sqlite3_close(dest);
        }
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to open backup destination '" + destination_path + "': " + message,
            .module = "Core.Database",
        });
    }

    sqlite3_backup* backup = sqlite3_backup_init(dest, "main", handle_, "main");
    if (backup == nullptr) {
        const std::string message = sqlite3_errmsg(dest);
        sqlite3_close(dest);
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "failed to initialize backup: " + message,
            .module = "Core.Database",
        });
    }
    sqlite3_backup_step(backup, -1);
    const int rc = sqlite3_backup_finish(backup);
    sqlite3_close(dest);
    if (rc != SQLITE_OK) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::Internal,
            .message = "backup failed with sqlite error code " + std::to_string(rc),
            .module = "Core.Database",
        });
    }
    return Result<void>::Ok();
}

std::string AsString(const SqlValue& value) {
    if (std::holds_alternative<std::string>(value)) return std::get<std::string>(value);
    if (std::holds_alternative<std::int64_t>(value)) return std::to_string(std::get<std::int64_t>(value));
    if (std::holds_alternative<double>(value)) return std::to_string(std::get<double>(value));
    return "";
}

std::int64_t AsInt64(const SqlValue& value) {
    if (std::holds_alternative<std::int64_t>(value)) return std::get<std::int64_t>(value);
    if (std::holds_alternative<double>(value)) return static_cast<std::int64_t>(std::get<double>(value));
    return 0;
}

double AsDouble(const SqlValue& value) {
    if (std::holds_alternative<double>(value)) return std::get<double>(value);
    if (std::holds_alternative<std::int64_t>(value)) return static_cast<double>(std::get<std::int64_t>(value));
    return 0.0;
}

} // namespace aistudio::core
