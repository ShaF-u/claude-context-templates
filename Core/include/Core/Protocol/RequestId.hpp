#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace aistudio::core {

// Monotonic id generator for Command/Query/Event correlation. Not a UUID —
// unique only within a single Core process, which is sufficient until a
// distributed Core (docs/ROADMAP.md Phase 10: Web/Mobile/Remote) needs
// global uniqueness. Kept dependency-free per docs/DEPENDENCY_MANAGEMENT.md.
class RequestIdGenerator {
public:
    static std::string Next();

private:
    static std::atomic<std::uint64_t> counter_;
};

} // namespace aistudio::core
