#include "Core/Backend/CapabilityVersion.hpp"

#include <array>
#include <charconv>
#include <cstddef>

namespace aistudio::core {

namespace {

std::optional<int> ParseNonNegativeInt(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || ptr != text.data() + text.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

std::vector<std::string> SplitDots(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (const char c : text) {
        if (c == '.') {
            parts.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    parts.push_back(current);
    return parts;
}

} // namespace

std::string CapabilityVersion::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}

std::optional<CapabilityVersion> CapabilityVersion::Parse(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    const auto parts = SplitDots(text);
    if (parts.empty() || parts.size() > 3) {
        return std::nullopt;
    }

    CapabilityVersion version;
    const std::array<int*, 3> fields = {&version.major, &version.minor, &version.patch};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto value = ParseNonNegativeInt(parts[i]);
        if (!value) {
            return std::nullopt;
        }
        *fields[i] = *value;
    }
    return version;
}

bool operator==(const CapabilityVersion& a, const CapabilityVersion& b) {
    return a.major == b.major && a.minor == b.minor && a.patch == b.patch;
}

bool operator<(const CapabilityVersion& a, const CapabilityVersion& b) {
    if (a.major != b.major) {
        return a.major < b.major;
    }
    if (a.minor != b.minor) {
        return a.minor < b.minor;
    }
    return a.patch < b.patch;
}

Capability Capability::Parse(const std::string& raw) {
    const auto at = raw.find('@');
    if (at == std::string::npos) {
        return Capability{raw, std::nullopt};
    }

    const std::string name = raw.substr(0, at);
    const std::string version_text = raw.substr(at + 1);
    if (const auto version = CapabilityVersion::Parse(version_text)) {
        return Capability{name, version};
    }
    // Text after '@' doesn't parse as a version — treat the whole raw
    // string as an opaque, unversioned name rather than silently
    // dropping the suffix.
    return Capability{raw, std::nullopt};
}

std::vector<Capability> ParseCapabilities(const std::vector<std::string>& raw_capabilities) {
    std::vector<Capability> result;
    result.reserve(raw_capabilities.size());
    for (const auto& raw : raw_capabilities) {
        result.push_back(Capability::Parse(raw));
    }
    return result;
}

bool IsCompatible(const std::optional<CapabilityVersion>& required, const std::optional<CapabilityVersion>& provided) {
    if (!required) {
        return true;
    }
    if (!provided) {
        return false;
    }
    if (provided->major != required->major) {
        return false;
    }
    return !(*provided < *required); // provided >= required within the same major version
}

} // namespace aistudio::core
