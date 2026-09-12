#include "Core/Config/Config.hpp"

#include <fstream>

namespace aistudio::core {

namespace {
std::string Trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}
} // namespace

Result<void> Config::LoadFromFile(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to open config file: " + path,
            .module = "Core.Config",
            .retryable = false,
        });
    }

    std::string line;
    while (std::getline(in, line)) {
        const auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#' || trimmed.front() == ';') {
            continue;
        }
        const auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        Set(Trim(trimmed.substr(0, eq)), Trim(trimmed.substr(eq + 1)));
    }
    return Result<void>::Ok();
}

Result<void> Config::SaveToFile(const std::string& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::IOError,
            .message = "failed to open config file for write: " + path,
            .module = "Core.Config",
            .retryable = true,
        });
    }
    for (const auto& [key, value] : values_) {
        out << key << "=" << value << "\n";
    }
    return Result<void>::Ok();
}

void Config::Set(const std::string& key, std::string value) {
    secret_values_.erase(key);
    values_[key] = std::move(value);
}

std::optional<std::string> Config::Get(const std::string& key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

std::string Config::GetOr(const std::string& key, std::string fallback) const {
    return Get(key).value_or(std::move(fallback));
}

void Config::SetSecret(const std::string& key, std::string value) {
    values_.erase(key);
    secret_values_[key] = std::move(value);
}

std::optional<Secret> Config::GetSecret(const std::string& key) const {
    const auto it = secret_values_.find(key);
    if (it == secret_values_.end()) return std::nullopt;
    return Secret(it->second);
}

bool Config::IsSecret(const std::string& key) const {
    return secret_values_.contains(key);
}

} // namespace aistudio::core
