#include "Core/Database/StringList.hpp"

#include <sstream>

namespace aistudio::core {

std::string JoinStringList(const std::vector<std::string>& items) {
    std::ostringstream oss;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            oss << ',';
        }
        oss << items[i];
    }
    return oss.str();
}

std::vector<std::string> SplitStringList(const std::string& joined) {
    std::vector<std::string> result;
    if (joined.empty()) {
        return result;
    }
    std::stringstream ss(joined);
    std::string item;
    while (std::getline(ss, item, ',')) {
        if (!item.empty()) {
            result.push_back(item);
        }
    }
    return result;
}

} // namespace aistudio::core
