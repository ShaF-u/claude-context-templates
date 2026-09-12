#pragma once

#include <string>

namespace aistudio::core {

enum class ErrorCode {
    Unknown = 0,
    NotFound,
    InvalidArgument,
    IOError,
    ParseError,
    Timeout,
    PermissionDenied,
    Cancelled,
    Internal,
};

[[nodiscard]] inline std::string ToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::NotFound: return "NotFound";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::IOError: return "IOError";
        case ErrorCode::ParseError: return "ParseError";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::PermissionDenied: return "PermissionDenied";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::Internal: return "Internal";
    }
    return "Unknown";
}

// Carries enough context to diagnose and recover from a failure without
// re-deriving it from logs. See AGENT.md #10 (error handling) and
// docs/DEVELOPMENT_PROTOCOL.md #7 (autonomous retry policy).
struct Error {
    ErrorCode code = ErrorCode::Unknown;
    std::string message;
    std::string module;
    std::string cause;
    bool retryable = false;

    [[nodiscard]] std::string ToString() const {
        std::string result = "[" + module + "] " + message;
        if (!cause.empty()) {
            result += " (cause: " + cause + ")";
        }
        return result;
    }
};

} // namespace aistudio::core
