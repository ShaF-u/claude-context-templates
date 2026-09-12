#pragma once

#include "Core/Backend/IBackend.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace aistudio::core {

// docs/MASTER_SPEC.md #25 "Project Rules": conventions the AI shouldn't
// need re-explained every session ("C++: raw pointer禁止" / "Git: small
// commits", etc). This project already has exactly that mechanism —
// CLAUDE.md/AGENT.md, read automatically by Claude Code itself — so
// rather than invent a second, competing rules format, this Backend
// simply exposes the SAME configured files to any MCP client (not every
// connecting AI is Claude Code, and not every one reads CLAUDE.md on its
// own).
//
// Unlike FileProviderBackend/GitBackend, ProvideContext() here ignores
// `intent` beyond requiring it be non-empty (still returns the SAME
// content regardless of what intent asked for) — rules are meant to
// apply universally, not be retrieved only when they happen to match a
// query.
class ProjectRulesBackend final : public IBackend {
public:
    [[nodiscard]] std::string Id() const override { return "core.project_rules"; }
    [[nodiscard]] std::string Name() const override { return "Project Rules Backend"; }
    [[nodiscard]] std::string Version() const override { return "0.1.0"; }
    [[nodiscard]] std::vector<std::string> Capabilities() const override {
        return {"rules.get@1.0.0", "context.provide.rules@1.0.0"};
    }

    // Always Healthy -- like FileProviderBackend, this has no external
    // dependency; configured files simply not existing is a normal,
    // harmless state (not every project has rules yet), not degraded
    // health.
    [[nodiscard]] BackendHealth Health() const override { return BackendHealth::Healthy; }
    Result<void> Start() override { return Result<void>::Ok(); }
    Result<void> Stop() override { return Result<void>::Ok(); }

    // Reads `backend.core.project_rules.root` (falling back to
    // `project.root`, then ".") and `backend.core.project_rules.files`
    // (comma-separated relative paths, default "CLAUDE.md,AGENT.md").
    // Each configured file that doesn't exist is silently skipped --
    // most projects won't have all of them.
    Result<void> Configure(const Config& config) override;

    // No commands -- read-only.
    CommandResult Dispatch(const Command& command) override;

    // "rules.get" (no parameters) -- the combined content of every
    // configured file that exists, each preceded by a "=== <path> ==="
    // divider.
    QueryResult Handle(const Query& query) override;

    [[nodiscard]] std::vector<ContextItem> ProvideContext(const std::string& intent) const override;

private:
    std::string root_;
    std::vector<std::string> files_;
};

} // namespace aistudio::core
