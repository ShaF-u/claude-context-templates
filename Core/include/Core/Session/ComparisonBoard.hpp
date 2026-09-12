#pragma once

#include "Core/Error/Result.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-8. 複数AIの比較・採用" -- deliberately a
// thin layer over 13-3 (worktree isolation) / 13-6 (HandoffRecord) /
// 13-7 (OperationLedger), not a new subsystem: a trial holds references
// into those (workspace_path, handoff_record_id, operation_id) rather
// than duplicating diffs/test output/state into itself.
enum class AdoptionDecision { Undecided, Adopted, Rejected };

[[nodiscard]] std::string ToString(AdoptionDecision decision);
[[nodiscard]] AdoptionDecision AdoptionDecisionFromString(const std::string& text);

// One CLI-type AI's independent attempt at a shared task, from a shared
// base commit, in its own worktree (docs/ROADMAP.md 13-3). Comparable
// trials share the same comparison_id.
struct ComparisonTrial {
    std::string id;
    std::string comparison_id;
    std::string task_id;
    std::string base_commit_sha;
    std::string cli_name;
    std::string session_id; // 13-2 Session this trial ran as, empty if none
    std::string workspace_path; // 13-3 worktree path -- diff is read from here, not copied in
    std::string handoff_record_id; // 13-6 reference, empty until that AI files one
    std::string operation_id; // 13-7 OperationLedger id tracking this trial's own completion, empty if none

    std::int64_t started_at = 0;
    std::int64_t finished_at = 0; // 0 = not yet finished

    // Never set directly by RegisterTrial() -- only DecideTrial() below
    // writes these three fields.
    AdoptionDecision decision = AdoptionDecision::Undecided;
    std::string decision_rationale;
    std::int64_t decided_at = 0;

    [[nodiscard]] std::optional<std::int64_t> DurationSeconds() const {
        if (finished_at <= 0) {
            return std::nullopt;
        }
        return finished_at - started_at;
    }
};

// Synchronous, no background thread (same shape as TaskQueue/
// SessionManager/ResourceLeaseLedger/OperationLedger).
//
// Enforcement limits, stated plainly (AGENT.md #15 -- don't claim a
// guarantee this doesn't actually have): unlike HandoffVerifier's
// verified_* fields (which only a real subprocess execution can
// populate), nothing in this class's C++ type system stops any caller,
// including an AI, from calling DecideTrial() itself. The "ユーザーが
// 採用を判断する" requirement is upheld in v1 as a wiring convention --
// this call is meant to be reachable only from a user-driven GUI action
// -- not as a mechanism this class enforces. That wiring is out of scope
// here (no GUI consumer exists yet); this is recorded honestly rather
// than left implied.
class ComparisonBoard {
public:
    // Fails if `trial.id` is already registered.
    Result<void> RegisterTrial(ComparisonTrial trial);

    [[nodiscard]] Result<std::vector<ComparisonTrial>> TrialsForComparison(const std::string& comparison_id) const;
    [[nodiscard]] std::optional<ComparisonTrial> Find(const std::string& trial_id) const;

    // The sole write path for a decision. `decision` must be Adopted or
    // Rejected (Undecided is rejected -- this call is for recording a
    // made decision, not un-making one). `rationale` must be non-empty
    // for both outcomes -- ROADMAP's "不採用の試行も...判断根拠を確認で
    // きるようにする" is treated as applying symmetrically: why a trial
    // was preferred is as much a judgment call worth recording as why
    // one was passed over. Deliberately no method here counts trials or
    // looks for agreement across their outputs -- adoption is always
    // exactly one explicit call, never derived from how many trials
    // reached the same answer (ROADMAP's "複数AIの同意だけを正しさの根
    // 拠にしない").
    Result<void> DecideTrial(const std::string& trial_id, AdoptionDecision decision, const std::string& rationale);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ComparisonTrial> trials_;
};

} // namespace aistudio::core
