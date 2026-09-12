#include "Core/Logging/Logger.hpp"

#include "Core/Util/Utf8.hpp"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace aistudio::core {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

void Logger::SetMinLevel(LogLevel level) {
    std::lock_guard lock(mutex_);
    min_level_ = level;
}

void Logger::SetLogFile(const std::string& path) {
    std::lock_guard lock(mutex_);
    // path is UTF-8; Utf8ToPath avoids CP_ACP corruption on Windows.
    file_sink_.open(Utf8ToPath(path), std::ios::app);
}

void Logger::SetConsoleEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    console_enabled_ = enabled;
}

std::string_view Logger::LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "UNKNOWN";
}

void Logger::Log(LogLevel level, std::string_view module, std::string_view message,
                  const std::source_location& location) {
    if (level < min_level_) {
        return;
    }

    const auto now = std::chrono::system_clock::now();
    const std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now_c);
#else
    localtime_r(&now_c, &tm);
#endif

    std::ostringstream line;
    line << "[" << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] "
         << "[" << LevelToString(level) << "] "
         << "[" << module << "] "
         << message
         << " (" << location.file_name() << ":" << location.line() << ")";

    std::lock_guard lock(mutex_);
    // stderr never interferes with a stdio-framed protocol on stdout
    // (Core/MCP/McpServer), so Error/Fatal keeps reaching it regardless
    // of console_enabled_ — only the stdout branch is what that flag
    // exists to silence.
    const bool is_error = level >= LogLevel::Error;
    if (is_error || console_enabled_) {
        std::ostream& out = is_error ? std::cerr : std::cout;
        out << line.str() << std::endl;
    }
    if (file_sink_.is_open()) {
        file_sink_ << line.str() << std::endl;
    }
}

} // namespace aistudio::core
