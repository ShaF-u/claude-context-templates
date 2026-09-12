#pragma once

#include <any>
#include <string>

namespace aistudio::core {

// A directive sent to a Backend to perform an action (a write), as
// opposed to a Query (a read). Every Backend talks to Core only through
// Command/Query/Event/Result — never through Backend-specific internals
// (AGENT.md #2, docs/MASTER_SPEC.md #31, #34).
struct Command {
    std::string request_id;
    std::string correlation_id;
    std::string backend_id;
    std::string name;
    std::any payload;
};

} // namespace aistudio::core
