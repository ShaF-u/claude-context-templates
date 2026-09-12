#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aistudio::core {

// A capability's semantic version — docs/ROADMAP.md "Capability System"
// versioning/negotiation (docs/MASTER_SPEC.md #32). Most Backends still
// advertise a bare capability name (IBackend::Capabilities()) with no
// version at all, exactly as before this was added — opting a specific
// capability into versioning just means appending "@major.minor.patch"
// to its name, e.g. "blueprint.read@2.1.0".
struct CapabilityVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;

    [[nodiscard]] std::string ToString() const;

    // Parses "major[.minor[.patch]]" — missing trailing parts default to
    // 0 (e.g. "2" -> {2,0,0}, "2.1" -> {2,1,0}). nullopt if any part
    // present isn't a non-negative integer, there are more than three
    // parts, or the text is empty.
    [[nodiscard]] static std::optional<CapabilityVersion> Parse(const std::string& text);
};

[[nodiscard]] bool operator==(const CapabilityVersion& a, const CapabilityVersion& b);
[[nodiscard]] bool operator<(const CapabilityVersion& a, const CapabilityVersion& b);

// One capability a Backend advertises, split into its bare name and
// optional version suffix (see CapabilityVersion above). A raw string
// with no "@", or whose text after "@" doesn't parse as a version, keeps
// its whole original text as `name` with a nullopt `version` — so every
// existing unversioned Capabilities() string still parses to exactly the
// name it already was.
struct Capability {
    std::string name;
    std::optional<CapabilityVersion> version;

    [[nodiscard]] static Capability Parse(const std::string& raw);
};

[[nodiscard]] std::vector<Capability> ParseCapabilities(const std::vector<std::string>& raw_capabilities);

// Negotiation rule: `required` (what a caller needs) is satisfied by
// `provided` (what a Backend advertises) when they share the same major
// version and `provided` is >= `required` within it — the common
// "backward compatible within a major version" SemVer convention. A
// nullopt `required` (caller doesn't care about version) is always
// satisfied. A nullopt `provided` (Backend didn't declare one) satisfies
// only a nullopt `required` — an unversioned Backend can't claim to meet
// a specific version requirement.
[[nodiscard]] bool IsCompatible(const std::optional<CapabilityVersion>& required,
                                 const std::optional<CapabilityVersion>& provided);

} // namespace aistudio::core
