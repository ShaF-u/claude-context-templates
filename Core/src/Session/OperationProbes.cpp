#include "Core/Session/OperationProbes.hpp"

namespace aistudio::core {

std::function<Result<OperationOutcome>()> SessionOutcomeProbe(const SessionManager& manager, std::string session_id) {
    return [&manager, session_id = std::move(session_id)]() -> Result<OperationOutcome> {
        const auto session = manager.Find(session_id);
        if (!session.has_value()) {
            return Result<OperationOutcome>::Fail(Error{.code = ErrorCode::NotFound,
                                                          .message = "session '" + session_id + "' is not tracked",
                                                          .module = "Core.Session.OperationProbes"});
        }
        if (session->state == SessionState::Terminated) {
            return Result<OperationOutcome>::Ok(OperationOutcome::Succeeded);
        }
        if (session->state == SessionState::Failed) {
            return Result<OperationOutcome>::Ok(OperationOutcome::Failed);
        }
        return Result<OperationOutcome>::Ok(OperationOutcome::Unknown);
    };
}

} // namespace aistudio::core
