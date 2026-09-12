#include "Core/Session/ComparisonBoard.hpp"

#include "Core/Util/Time.hpp"

namespace aistudio::core {

std::string ToString(AdoptionDecision decision) {
    switch (decision) {
        case AdoptionDecision::Undecided: return "Undecided";
        case AdoptionDecision::Adopted: return "Adopted";
        case AdoptionDecision::Rejected: return "Rejected";
    }
    return "Undecided";
}

AdoptionDecision AdoptionDecisionFromString(const std::string& text) {
    if (text == "Adopted") return AdoptionDecision::Adopted;
    if (text == "Rejected") return AdoptionDecision::Rejected;
    return AdoptionDecision::Undecided; // "Undecided" itself, and any unrecognized value.
}

Result<void> ComparisonBoard::RegisterTrial(ComparisonTrial trial) {
    std::lock_guard lock(mutex_);
    if (trials_.find(trial.id) != trials_.end()) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                         .message = "trial '" + trial.id + "' is already registered",
                                         .module = "Core.Session.ComparisonBoard"});
    }
    const std::string id = trial.id;
    trials_.emplace(id, std::move(trial));
    return Result<void>::Ok();
}

Result<std::vector<ComparisonTrial>> ComparisonBoard::TrialsForComparison(const std::string& comparison_id) const {
    std::lock_guard lock(mutex_);
    std::vector<ComparisonTrial> result;
    for (const auto& [id, trial] : trials_) {
        if (trial.comparison_id == comparison_id) {
            result.push_back(trial);
        }
    }
    return Result<std::vector<ComparisonTrial>>::Ok(std::move(result));
}

std::optional<ComparisonTrial> ComparisonBoard::Find(const std::string& trial_id) const {
    std::lock_guard lock(mutex_);
    const auto it = trials_.find(trial_id);
    if (it == trials_.end()) {
        return std::nullopt;
    }
    return it->second;
}

Result<void> ComparisonBoard::DecideTrial(const std::string& trial_id, AdoptionDecision decision,
                                           const std::string& rationale) {
    if (decision == AdoptionDecision::Undecided) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                         .message = "DecideTrial() requires Adopted or Rejected, not Undecided, "
                                                     "for trial '" + trial_id + "'",
                                         .module = "Core.Session.ComparisonBoard"});
    }
    if (rationale.empty()) {
        return Result<void>::Fail(Error{.code = ErrorCode::InvalidArgument,
                                         .message = "DecideTrial() requires a non-empty rationale for trial '" +
                                                     trial_id + "'",
                                         .module = "Core.Session.ComparisonBoard"});
    }

    std::lock_guard lock(mutex_);
    const auto it = trials_.find(trial_id);
    if (it == trials_.end()) {
        return Result<void>::Fail(Error{.code = ErrorCode::NotFound,
                                         .message = "trial '" + trial_id + "' is not tracked",
                                         .module = "Core.Session.ComparisonBoard"});
    }
    it->second.decision = decision;
    it->second.decision_rationale = rationale;
    it->second.decided_at = CurrentUnixTimestamp();
    return Result<void>::Ok();
}

} // namespace aistudio::core
