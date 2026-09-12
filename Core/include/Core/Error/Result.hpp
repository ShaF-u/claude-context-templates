#pragma once

#include "Core/Error/Error.hpp"

#include <cassert>
#include <optional>
#include <utility>
#include <variant>

namespace aistudio::core {

// Explicit success/failure return type used across Core instead of
// exceptions or out-parameters, so every fallible call states its
// failure mode at the type level (AGENT.md #10, #14).
template <typename T>
class Result {
public:
    static Result Ok(T value) { return Result(std::move(value)); }
    static Result Fail(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool IsOk() const noexcept { return std::holds_alternative<T>(storage_); }
    [[nodiscard]] bool IsError() const noexcept { return !IsOk(); }
    explicit operator bool() const noexcept { return IsOk(); }

    [[nodiscard]] const T& Value() const& { assert(IsOk()); return std::get<T>(storage_); }
    [[nodiscard]] T& Value() & { assert(IsOk()); return std::get<T>(storage_); }
    [[nodiscard]] T Value() && { assert(IsOk()); return std::move(std::get<T>(storage_)); }

    [[nodiscard]] const Error& Err() const& { assert(IsError()); return std::get<Error>(storage_); }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Error error) : storage_(std::move(error)) {}

    std::variant<T, Error> storage_;
};

// Specialization for operations that either succeed with no payload or fail.
template <>
class Result<void> {
public:
    static Result Ok() { return Result(std::nullopt); }
    static Result Fail(Error error) { return Result(std::move(error)); }

    [[nodiscard]] bool IsOk() const noexcept { return !error_.has_value(); }
    [[nodiscard]] bool IsError() const noexcept { return error_.has_value(); }
    explicit operator bool() const noexcept { return IsOk(); }

    [[nodiscard]] const Error& Err() const& { assert(IsError()); return *error_; }

private:
    explicit Result(std::optional<Error> error) : error_(std::move(error)) {}
    std::optional<Error> error_;
};

} // namespace aistudio::core
