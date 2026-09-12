#pragma once

#include "Core/Backend/CapabilityVersion.hpp"
#include "Core/Backend/IBackend.hpp"
#include "Core/Config/Config.hpp"
#include "Core/Protocol/Protocol.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// A Backend's Health() reading before/after one CheckHealth() poll. Only
// entries whose reading actually changed are published as
// "BackendHealthChanged" events — polling itself is silent when nothing
// changed, so listeners aren't flooded (AGENT.md #9 — observable, not
// noisy).
struct BackendHealthChange {
    std::string backend_id;
    BackendHealth previous;
    BackendHealth current;
};

// Published as a "BackendLifecycleChanged" event for every transition a
// StartBackend()/StopBackend() call makes (Registered->Starting,
// Starting->Running or ->Failed, Running->Stopping, Stopping->Stopped or
// ->Failed) — unlike BackendHealthChange, every call always changes
// state, so there's no "only if changed" filtering here.
struct BackendLifecycleChange {
    std::string backend_id;
    BackendLifecycleState previous;
    BackendLifecycleState current;
};

// Tracks every registered Backend by id and resolves Backends by the
// Capability they declare, so Core selects Backends by what they can do
// rather than by hardcoded name (docs/MASTER_SPEC.md #32-33).
class BackendRegistry {
public:
    Result<void> Register(std::shared_ptr<IBackend> backend);
    void Unregister(const std::string& id);

    [[nodiscard]] std::shared_ptr<IBackend> Find(const std::string& id) const;

    // Resolves Backends by capability NAME, optionally negotiating a
    // minimum version (see CapabilityVersion::IsCompatible) — each of a
    // Backend's Capabilities() strings is parsed (Capability::Parse) and
    // matched by name; `required_version` left at nullopt (the default)
    // matches any version, including an unversioned one, so existing
    // call sites that only ever cared about the name keep working
    // unchanged.
    [[nodiscard]] std::vector<std::shared_ptr<IBackend>> FindByCapability(
        const std::string& capability_name, std::optional<CapabilityVersion> required_version = std::nullopt) const;

    [[nodiscard]] std::vector<std::shared_ptr<IBackend>> All() const;

    // Routes a Command/Query to the Backend named by its backend_id,
    // filling in request_id if the caller left it blank. This is the
    // single entry point Core/Agent code should use instead of calling
    // Find() and dispatching manually (docs/MASTER_SPEC.md #34).
    // (Named RunQuery, not Query, to avoid colliding with the `Query` type.)
    [[nodiscard]] CommandResult Dispatch(Command command) const;
    [[nodiscard]] QueryResult RunQuery(Query query) const;

    // Calls Configure(config) on every registered Backend. Attempts all
    // of them even if one fails; the returned Result is Fail only if at
    // least one Backend failed, with every failure's message combined.
    Result<void> ConfigureAll(const Config& config) const;

    // Polls Health() on every registered Backend, publishing
    // "BackendHealthChanged" (BackendHealthChange) via EventBus for any
    // whose reading differs from the previous poll (or has no previous
    // reading yet). Returns every backend's current reading regardless.
    std::vector<std::pair<std::string, BackendHealth>> CheckHealth();

    // Drives one Backend through Registered/Stopped/Failed -> Starting ->
    // Running (or -> Failed if Start() itself fails), publishing
    // "BackendLifecycleChanged" for each transition. Fails without
    // calling Start() if the id is unknown or the Backend isn't in a
    // startable state (already Starting/Running/Stopping).
    Result<void> StartBackend(const std::string& id);

    // The Stop() counterpart: Running -> Stopping -> Stopped (or ->
    // Failed if Stop() itself fails). Fails without calling Stop() if the
    // id is unknown or the Backend isn't Running.
    Result<void> StopBackend(const std::string& id);

    // Calls StartBackend()/StopBackend() on every registered Backend.
    // Attempts all of them even if one fails; the returned Result is
    // Fail only if at least one Backend failed, with every failure's
    // message combined (same aggregation style as ConfigureAll).
    Result<void> StartAll();
    Result<void> StopAll();

    // nullopt if `id` has never been registered; Registered for a
    // Backend that's never had StartBackend() called on it.
    [[nodiscard]] std::optional<BackendLifecycleState> LifecycleState(const std::string& id) const;

private:
    void TransitionTo(const std::string& id, BackendLifecycleState new_state);

    std::unordered_map<std::string, std::shared_ptr<IBackend>> backends_;
    std::unordered_map<std::string, BackendHealth> last_health_;
    std::unordered_map<std::string, BackendLifecycleState> lifecycle_states_;
};

} // namespace aistudio::core
