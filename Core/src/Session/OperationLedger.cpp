#include "Core/Session/OperationLedger.hpp"

#include "Core/Util/Time.hpp"

namespace aistudio::core {

std::string ToString(OperationOutcome outcome) {
    switch (outcome) {
        case OperationOutcome::Unknown: return "Unknown";
        case OperationOutcome::Succeeded: return "Succeeded";
        case OperationOutcome::Failed: return "Failed";
    }
    return "Unknown";
}

OperationOutcome OperationOutcomeFromString(const std::string& text) {
    if (text == "Succeeded") return OperationOutcome::Succeeded;
    if (text == "Failed") return OperationOutcome::Failed;
    return OperationOutcome::Unknown; // "Unknown" itself, and any unrecognized value.
}

Result<OperationBeginOutcome> OperationLedger::Begin(const std::string& id, const std::string& kind) {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end()) {
        records_[id] = OperationRecord{id, kind, CurrentUnixTimestamp(), 0, OperationOutcome::Unknown, ""};
        return Result<OperationBeginOutcome>::Ok(OperationBeginOutcome::Started);
    }

    OperationRecord& record = it->second;
    if (record.outcome == OperationOutcome::Succeeded) {
        return Result<OperationBeginOutcome>::Ok(OperationBeginOutcome::AlreadyCompleted);
    }
    if (record.outcome == OperationOutcome::Unknown) {
        return Result<OperationBeginOutcome>::Ok(OperationBeginOutcome::NeedsReconciliation);
    }
    // Failed -- a definite result, safe to retry under the same id.
    record.kind = kind;
    record.started_at = CurrentUnixTimestamp();
    record.finished_at = 0;
    record.outcome = OperationOutcome::Unknown;
    record.interruption_reason.clear();
    return Result<OperationBeginOutcome>::Ok(OperationBeginOutcome::Started);
}

Result<void> OperationLedger::Complete(const std::string& id, OperationOutcome outcome) {
    if (outcome == OperationOutcome::Unknown) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                         .message = "Complete() requires a definite outcome (Succeeded or Failed), "
                                                     "not Unknown, for operation '" + id + "'",
                                         .module = "Core.Session.OperationLedger"});
    }
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end()) {
        return Result<void>::Fail(Error{.code = ErrorCode::NotFound,
                                         .message = "operation '" + id + "' is not tracked",
                                         .module = "Core.Session.OperationLedger"});
    }
    it->second.finished_at = CurrentUnixTimestamp();
    it->second.outcome = outcome;
    return Result<void>::Ok();
}

Result<void> OperationLedger::MarkInterrupted(const std::string& id, const std::string& reason) {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end()) {
        return Result<void>::Fail(Error{.code = ErrorCode::NotFound,
                                         .message = "operation '" + id + "' is not tracked",
                                         .module = "Core.Session.OperationLedger"});
    }
    it->second.finished_at = CurrentUnixTimestamp();
    it->second.interruption_reason = reason;
    // outcome stays Unknown -- an observed interruption is still not a
    // known success/failure.
    return Result<void>::Ok();
}

Result<OperationOutcome> OperationLedger::Reconcile(const std::string& id,
                                                     const std::function<Result<OperationOutcome>()>& probe) {
    {
        std::lock_guard lock(mutex_);
        if (records_.find(id) == records_.end()) {
            return Result<OperationOutcome>::Fail(Error{.code = ErrorCode::NotFound,
                                                          .message = "operation '" + id + "' is not tracked",
                                                          .module = "Core.Session.OperationLedger"});
        }
    }

    const auto probed = probe();
    if (!probed) {
        return Result<OperationOutcome>::Fail(probed.Err());
    }

    if (probed.Value() == OperationOutcome::Unknown) {
        return Result<OperationOutcome>::Ok(OperationOutcome::Unknown);
    }

    const auto complete_result = Complete(id, probed.Value());
    if (!complete_result) {
        return Result<OperationOutcome>::Fail(complete_result.Err());
    }
    return Result<OperationOutcome>::Ok(probed.Value());
}

std::optional<OperationRecord> OperationLedger::Find(const std::string& id) const {
    std::lock_guard lock(mutex_);
    const auto it = records_.find(id);
    if (it == records_.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<OperationRecord> OperationLedger::All() const {
    std::lock_guard lock(mutex_);
    std::vector<OperationRecord> result;
    result.reserve(records_.size());
    for (const auto& [id, record] : records_) {
        result.push_back(record);
    }
    return result;
}

} // namespace aistudio::core
