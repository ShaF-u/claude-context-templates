#include "Core/Session/SessionManager.hpp"

#include "Core/Event/EventBus.hpp"
#include "Core/Util/Time.hpp"

namespace aistudio::core {

SessionManager::SessionManager(ResourceLeaseLedger* resource_lease_ledger)
    : resource_lease_ledger_(resource_lease_ledger) {}

Result<Session> SessionManager::CreateSession(Session session, const CliProfile& profile,
                                               std::unique_ptr<ICliAdapter> adapter) {
    if (session.id.empty()) {
        return Result<Session>::Fail(Error{
            .code = ErrorCode::InvalidArgument, .message = "session id must not be empty", .module = "Core.Session.SessionManager"});
    }
    {
        std::lock_guard lock(mutex_);
        if (sessions_.contains(session.id)) {
            return Result<Session>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                                .message = "session id already exists: " + session.id,
                                                .module = "Core.Session.SessionManager"});
        }
    }

    session.state = SessionState::Starting;
    session.created_at = CurrentUnixTimestamp();

    // Start() (a real process spawn) runs without mutex_ held -- holding
    // it here would block every other SessionManager call for as long as
    // this one adapter takes to launch.
    const bool started = adapter->Start(profile);
    session.started_at = CurrentUnixTimestamp();
    if (started) {
        session.state = SessionState::Running;
    } else {
        session.state = SessionState::Failed;
        session.ended_at = session.started_at;
    }

    {
        std::lock_guard lock(mutex_);
        // Re-check: another CreateSession() with the same id could have
        // raced in while Start() above was running unlocked.
        if (sessions_.contains(session.id)) {
            adapter->Stop();
            return Result<Session>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                                .message = "session id already exists: " + session.id,
                                                .module = "Core.Session.SessionManager"});
        }
        insertion_order_.push_back(session.id);
        sessions_.emplace(session.id, Entry{session, std::move(adapter), false});
    }
    EventBus::Instance().Publish("SessionStateChanged", session);
    return Result<Session>::Ok(session);
}

Result<void> SessionManager::Stop(const std::string& session_id) {
    std::optional<Session> updated;
    {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::NotFound, .message = "no such session: " + session_id, .module = "Core.Session.SessionManager"});
        }
        if (it->second.restart_in_progress) {
            return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                             .message = "session is mid-restart, cannot stop concurrently: " + session_id,
                                             .module = "Core.Session.SessionManager"});
        }
        it->second.stop_requested = true;
        it->second.adapter->Stop();
        if (!it->second.session.IsTerminal()) {
            it->second.session.state = SessionState::Terminated;
            it->second.session.ended_at = CurrentUnixTimestamp();
            updated = it->second.session;
        }
    }
    if (updated.has_value()) {
        EventBus::Instance().Publish("SessionStateChanged", *updated);
        if (resource_lease_ledger_ != nullptr) {
            resource_lease_ledger_->ReleaseAllForSession(session_id);
        }
    }
    return Result<void>::Ok();
}

Result<void> SessionManager::Restart(const std::string& session_id, const CliProfile& profile) {
    ICliAdapter* adapter = nullptr;
    {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::NotFound, .message = "no such session: " + session_id, .module = "Core.Session.SessionManager"});
        }
        if (!it->second.session.IsTerminal()) {
            return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                             .message = "session is not terminal, cannot restart: " + session_id,
                                             .module = "Core.Session.SessionManager"});
        }
        adapter = it->second.adapter.get();
        it->second.stop_requested = false;
        it->second.restart_in_progress = true;
    }

    // Start() (a real process spawn) runs without mutex_ held, same
    // reasoning as CreateSession() -- `adapter` itself stays valid
    // unlocked since SessionManager never erases an entry once inserted
    // (no RemoveSession() exists), so nothing can invalidate what it
    // points to while this runs. restart_in_progress (set above) makes
    // Stop() reject for this session_id until the block below clears it,
    // so adapter->Stop() can never run concurrently with this Start().
    const bool started = adapter->Start(profile);
    const std::int64_t now = CurrentUnixTimestamp();

    Session updated;
    {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return Result<void>::Fail(Error{.code = ErrorCode::NotFound,
                                             .message = "session disappeared during restart: " + session_id,
                                             .module = "Core.Session.SessionManager"});
        }
        it->second.restart_in_progress = false;
        it->second.session.started_at = now;
        if (started) {
            it->second.session.state = SessionState::Running;
            it->second.session.ended_at = 0;
        } else {
            it->second.session.state = SessionState::Failed;
            it->second.session.ended_at = now;
        }
        updated = it->second.session;
    }
    EventBus::Instance().Publish("SessionStateChanged", updated);
    return Result<void>::Ok();
}

Result<void> SessionManager::SetState(const std::string& session_id, SessionState state) {
    if (state == SessionState::Terminated || state == SessionState::Failed) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                         .message = "SetState cannot set a terminal state directly -- "
                                                     "only PollAll()/Stop() do, since only they consult ExitCode()",
                                         .module = "Core.Session.SessionManager"});
    }
    std::optional<Session> updated;
    {
        std::lock_guard lock(mutex_);
        const auto it = sessions_.find(session_id);
        if (it == sessions_.end()) {
            return Result<void>::Fail(Error{
                .code = ErrorCode::NotFound, .message = "no such session: " + session_id, .module = "Core.Session.SessionManager"});
        }
        if (it->second.session.IsTerminal()) {
            return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                             .message = "session already terminal: " + session_id,
                                             .module = "Core.Session.SessionManager"});
        }
        it->second.session.state = state;
        updated = it->second.session;
    }
    EventBus::Instance().Publish("SessionStateChanged", *updated);
    return Result<void>::Ok();
}

std::vector<std::string> SessionManager::PollAll() {
    std::vector<Session> changed;
    {
        std::lock_guard lock(mutex_);
        for (const auto& id : insertion_order_) {
            const auto it = sessions_.find(id);
            if (it == sessions_.end()) {
                continue;
            }
            Entry& entry = it->second;
            if (entry.session.IsTerminal() || entry.adapter->IsRunning()) {
                continue;
            }
            const auto exit_code = entry.adapter->ExitCode();
            const bool clean = entry.stop_requested || (exit_code.has_value() && exit_code.value() == 0);
            entry.session.state = clean ? SessionState::Terminated : SessionState::Failed;
            entry.session.ended_at = CurrentUnixTimestamp();
            changed.push_back(entry.session);
        }
    }
    std::vector<std::string> changed_ids;
    changed_ids.reserve(changed.size());
    for (const auto& session : changed) {
        changed_ids.push_back(session.id);
        EventBus::Instance().Publish("SessionStateChanged", session);
        if (resource_lease_ledger_ != nullptr) {
            resource_lease_ledger_->ReleaseAllForSession(session.id);
        }
    }
    return changed_ids;
}

std::optional<Session> SessionManager::Find(const std::string& session_id) const {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    return it->second.session;
}

std::vector<Session> SessionManager::All() const {
    std::lock_guard lock(mutex_);
    std::vector<Session> result;
    result.reserve(insertion_order_.size());
    for (const auto& id : insertion_order_) {
        const auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            result.push_back(it->second.session);
        }
    }
    return result;
}

ICliAdapter* SessionManager::Adapter(const std::string& session_id) {
    std::lock_guard lock(mutex_);
    const auto it = sessions_.find(session_id);
    return it == sessions_.end() ? nullptr : it->second.adapter.get();
}

} // namespace aistudio::core
