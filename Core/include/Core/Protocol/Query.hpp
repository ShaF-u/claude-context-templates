#pragma once

#include <any>
#include <string>

namespace aistudio::core {

// A read-only request to a Backend. Never mutates Backend state — use
// Command for that (AGENT.md #2, docs/MASTER_SPEC.md #31).
struct Query {
    std::string request_id;
    std::string correlation_id;
    std::string backend_id;
    std::string name;
    std::any parameters;
};

} // namespace aistudio::core
