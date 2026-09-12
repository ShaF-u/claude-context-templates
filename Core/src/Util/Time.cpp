#include "Core/Util/Time.hpp"

#include <chrono>

namespace aistudio::core {

std::int64_t CurrentUnixTimestamp() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace aistudio::core
