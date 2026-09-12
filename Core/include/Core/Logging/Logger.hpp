#pragma once

#include <fstream>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>

namespace aistudio::core {

enum class LogLevel { Trace, Debug, Info, Warn, Error, Fatal };

// Minimal dependency-free logger for Phase 0. Meant to be swapped for
// (or wrapped by) a richer backend once Dependency Management
// (docs/DEPENDENCY_MANAGEMENT.md) brings in a package manager; call
// sites depend only on this interface, not the sink implementation.
class Logger {
public:
    static Logger& Instance();

    void SetMinLevel(LogLevel level);
    void SetLogFile(const std::string& path);
    // Off for a process whose stdout/stderr must stay reserved for
    // something other than log lines (Core/MCP/McpServer's stdio JSON-RPC
    // transport — any stray line on stdout breaks message framing for
    // the client). SetLogFile still captures everything for observability
    // (AGENT.md #9) even with this off.
    void SetConsoleEnabled(bool enabled);

    void Log(LogLevel level, std::string_view module, std::string_view message,
              const std::source_location& location = std::source_location::current());

private:
    Logger() = default;

    static std::string_view LevelToString(LogLevel level);

    std::mutex mutex_;
    LogLevel min_level_ = LogLevel::Info;
    bool console_enabled_ = true;
    std::ofstream file_sink_;
};

} // namespace aistudio::core

#define AISTUDIO_LOG_TRACE(module, msg) ::aistudio::core::Logger::Instance().Log(::aistudio::core::LogLevel::Trace, module, msg)
#define AISTUDIO_LOG_DEBUG(module, msg) ::aistudio::core::Logger::Instance().Log(::aistudio::core::LogLevel::Debug, module, msg)
#define AISTUDIO_LOG_INFO(module, msg)  ::aistudio::core::Logger::Instance().Log(::aistudio::core::LogLevel::Info,  module, msg)
#define AISTUDIO_LOG_WARN(module, msg)  ::aistudio::core::Logger::Instance().Log(::aistudio::core::LogLevel::Warn,  module, msg)
#define AISTUDIO_LOG_ERROR(module, msg) ::aistudio::core::Logger::Instance().Log(::aistudio::core::LogLevel::Error, module, msg)
#define AISTUDIO_LOG_FATAL(module, msg) ::aistudio::core::Logger::Instance().Log(::aistudio::core::LogLevel::Fatal, module, msg)
