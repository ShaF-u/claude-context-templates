#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Security/Secret.hpp"

#include <optional>
#include <string>
#include <unordered_map>

namespace aistudio::core {

// Minimal key=value configuration store (INI-like, no sections) used as
// Phase 0's configuration foundation. Intentionally dependency-free;
// once Dependency Management (docs/DEPENDENCY_MANAGEMENT.md) is wired up,
// this can grow into a JSON/TOML-backed implementation behind the same
// Get/Set/Load/Save surface so call sites do not need to change.
//
// Secret Protection (docs/MASTER_SPEC.md #73): a key set via SetSecret()
// is held in a separate map Get()/GetOr()/SaveToFile() never look at —
// it's retrievable only through GetSecret(), returning a Secret rather
// than a plain std::string, and SaveToFile() never persists it, so a
// value like an LLM provider API key (Phase 4) can't leak into a
// plaintext config file or get logged by a call site that only ever
// reaches for Get(). Set() and SetSecret() are mutually exclusive per
// key — whichever was called most recently wins, clearing the other
// map's entry for that key.
class Config {
public:
    Result<void> LoadFromFile(const std::string& path);
    Result<void> SaveToFile(const std::string& path) const;

    void Set(const std::string& key, std::string value);
    [[nodiscard]] std::optional<std::string> Get(const std::string& key) const;
    [[nodiscard]] std::string GetOr(const std::string& key, std::string fallback) const;
    // Every non-secret key/value pair -- for a caller that needs to
    // forward the whole config somewhere generic (e.g. serializing it to
    // JSON for a Plugin Backend across the ABI boundary, PluginBackendAdapter
    // ::Configure()) rather than reading specific known keys one at a
    // time the way IBackend::Configure() implementations normally do.
    // Secret values are never included here, same as SaveToFile().
    [[nodiscard]] const std::unordered_map<std::string, std::string>& All() const { return values_; }

    void SetSecret(const std::string& key, std::string value);
    [[nodiscard]] std::optional<Secret> GetSecret(const std::string& key) const;
    [[nodiscard]] bool IsSecret(const std::string& key) const;

private:
    std::unordered_map<std::string, std::string> values_;
    std::unordered_map<std::string, std::string> secret_values_;
};

} // namespace aistudio::core
