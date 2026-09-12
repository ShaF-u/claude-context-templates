#include "Core/Protocol/RequestId.hpp"

#include <sstream>

namespace aistudio::core {

std::atomic<std::uint64_t> RequestIdGenerator::counter_{0};

std::string RequestIdGenerator::Next() {
    const auto value = counter_.fetch_add(1, std::memory_order_relaxed);
    std::ostringstream oss;
    oss << "req-" << value;
    return oss.str();
}

} // namespace aistudio::core
