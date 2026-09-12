#pragma once

#include "Core/Session/ICliAdapter.hpp"
#include "Core/Terminal/PtyProcess.hpp"

namespace aistudio::core {

// The "非対応CLIはターミナル表示+手動操作へフォールバック" adapter
// (docs/ROADMAP.md 13-1): wraps PtyProcess directly with no CLI-specific
// knowledge, so every CliCapabilities field stays Unknown rather than
// guessed (AGENT.md #15). Works for any CLI, at the cost of exposing only
// what a raw terminal can: text in, text out.
class GenericCliAdapter : public ICliAdapter {
public:
    explicit GenericCliAdapter(std::string name);

    [[nodiscard]] const std::string& Name() const override;
    [[nodiscard]] const CliCapabilities& Capabilities() const override;

    [[nodiscard]] bool Start(const CliProfile& profile) override;
    void Stop() override;
    [[nodiscard]] bool IsRunning() override;
    [[nodiscard]] std::optional<int> ExitCode() const override;
    [[nodiscard]] const std::string& LastErrorMessage() const override;

    void SendInput(std::string_view bytes) override;
    [[nodiscard]] std::string TakeOutput() override;
    void Resize(std::int16_t cols, std::int16_t rows) override;

private:
    std::string name_;
    CliCapabilities capabilities_; // all Unknown, never set otherwise
    PtyProcess process_;
};

} // namespace aistudio::core
