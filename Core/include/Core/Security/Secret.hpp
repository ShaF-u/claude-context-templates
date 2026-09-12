#pragma once

#include <string>

namespace aistudio::core {

// A value that must never be accidentally logged, serialized, or
// otherwise exposed — docs/MASTER_SPEC.md #73 Security > "Secret
// Protection" (an LLM provider API key, Phase 4, is the concrete
// upcoming consumer; Config's SetSecret()/GetSecret() is the first
// thing this wires up). Wraps a std::string but deliberately has no
// implicit conversion to std::string — ToString() always returns a
// fixed redaction marker, never the real value. The only way to get the
// real value is the explicitly-named Reveal(), which is self-documenting
// and greppable ("show me every place that actually unwraps a secret")
// the way a bare .c_str() or an implicit conversion operator wouldn't be.
class Secret {
public:
    Secret() = default;
    explicit Secret(std::string value) : value_(std::move(value)) {}

    [[nodiscard]] const std::string& Reveal() const { return value_; }
    [[nodiscard]] bool Empty() const { return value_.empty(); }

    // Never the real value — see class comment above.
    [[nodiscard]] std::string ToString() const { return value_.empty() ? "" : "***REDACTED***"; }

private:
    std::string value_;
};

} // namespace aistudio::core
