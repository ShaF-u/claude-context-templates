#pragma once

#include <cstdint>

namespace aistudio::core {

// Current wall-clock time as unix seconds. Shared by anything that
// stamps a record with "when this happened" (ContextSnapshot,
// ContextAuditEntry, ...) instead of each call site repeating the same
// chrono boilerplate.
[[nodiscard]] std::int64_t CurrentUnixTimestamp();

} // namespace aistudio::core
