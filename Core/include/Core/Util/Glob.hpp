#pragma once

#include <string>

namespace aistudio::core {

// Minimal glob matcher: '*' matches any run of characters, everything
// else is literal. Shared by FileScanner's ignore patterns and
// PermissionPolicy's dangerous-command patterns rather than each
// reimplementing the same regex-translation logic.
[[nodiscard]] bool GlobMatch(const std::string& text, const std::string& pattern);

} // namespace aistudio::core
