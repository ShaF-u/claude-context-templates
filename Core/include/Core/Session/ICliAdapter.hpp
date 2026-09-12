#pragma once

#include "Core/Session/Capability.hpp"
#include "Core/Session/CliProfile.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aistudio::core {

// Per-CLI adapter (docs/ROADMAP.md Phase 13 "13-1"). Deliberately NOT an
// IBackend: a Backend is something Studio calls as a tool (Command/Query,
// Studio-initiated); a CLI-type AI is the reverse -- it's the one
// connecting in and initiating work, Studio only supervises its process
// and forwards input/output. Different lifecycle, different permission
// direction, so this doesn't share IBackend's shape.
class ICliAdapter {
public:
    virtual ~ICliAdapter() = default;

    [[nodiscard]] virtual const std::string& Name() const = 0;
    [[nodiscard]] virtual const CliCapabilities& Capabilities() const = 0;

    [[nodiscard]] virtual bool Start(const CliProfile& profile) = 0;
    virtual void Stop() = 0;
    [[nodiscard]] virtual bool IsRunning() = 0;
    // Set once the child is observed to have actually exited (see
    // PtyProcess::ExitCode()) -- nullopt while still running or never
    // started.
    [[nodiscard]] virtual std::optional<int> ExitCode() const = 0;
    [[nodiscard]] virtual const std::string& LastErrorMessage() const = 0;

    virtual void SendInput(std::string_view bytes) = 0;
    [[nodiscard]] virtual std::string TakeOutput() = 0;
    virtual void Resize(std::int16_t cols, std::int16_t rows) = 0;
};

} // namespace aistudio::core
