#pragma once

#include <string>

namespace aistudio::core {

// AGENT.md #15: don't fake support for a feature the adapter can't
// actually confirm -- Unknown is a real, distinct answer, not a
// placeholder to eventually replace.
enum class SupportLevel { Unknown, Supported, Unsupported };

[[nodiscard]] std::string ToString(SupportLevel level);

// What one ICliAdapter can confirm about the CLI it wraps
// (docs/ROADMAP.md 13-1's "Capability宣言" checklist item). A
// general-purpose adapter with no CLI-specific knowledge (see
// GenericCliAdapter) reports Unknown for all of these rather than
// guessing.
struct CliCapabilities {
    SupportLevel resume_conversation = SupportLevel::Unknown;
    SupportLevel cancel = SupportLevel::Unknown;
    SupportLevel approval_wait_detection = SupportLevel::Unknown;
    SupportLevel structured_output = SupportLevel::Unknown;
    SupportLevel usage_retrieval = SupportLevel::Unknown;
};

} // namespace aistudio::core
