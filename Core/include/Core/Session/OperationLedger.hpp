#pragma once

#include "Core/Error/Result.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-7. 中断後の復旧と二重実行防止" -- an
// idempotency + reconciliation ledger for operations that must not be
// silently re-run just because the process that started them didn't
// live to see them finish. Unknown is not a placeholder for Failed: an
// operation that started but was never observed to finish (crash,
// forced GUI exit, connection drop, OS restart -- this ledger can't
// itself tell which, see MarkInterrupted()) must stay distinguishable
// from one that is definitely known to have failed, or a caller that
// treats Unknown as Failed and retries risks running it twice.
enum class OperationOutcome { Unknown, Succeeded, Failed };

[[nodiscard]] std::string ToString(OperationOutcome outcome);
[[nodiscard]] OperationOutcome OperationOutcomeFromString(const std::string& text);

struct OperationRecord {
    std::string id;
    std::string kind;
    std::int64_t started_at = 0;
    std::int64_t finished_at = 0; // 0 = not yet finished (still Unknown)
    OperationOutcome outcome = OperationOutcome::Unknown;
    // Free text, filled in only when the interruption itself was
    // directly observed by a caller (e.g. the GUI's own shutdown path
    // calling MarkInterrupted() before exiting) -- distinguishing GUI
    // exit / CLI crash / connection drop / OS restart is not something
    // this ledger can detect on its own; a genuine crash simply leaves
    // this empty and the record stuck at Unknown until Reconcile()'d.
    std::string interruption_reason;
};

// What Begin() decided, so a caller can tell an operation it's safe to
// retry automatically from one it must not touch without checking
// reality first (ROADMAP's "自動再試行できる操作と、状態確認が必要な操作
// を区別する").
enum class OperationBeginOutcome {
    Started,             // fresh id, or a previous attempt definitely Failed -- safe to run again
    AlreadyCompleted,     // a previous attempt definitely Succeeded -- do not re-run
    NeedsReconciliation, // a previous attempt is still Unknown -- check reality (Reconcile()) before retrying
};

// Synchronous, no background thread (same shape as TaskQueue/
// SessionManager/ResourceLeaseLedger) -- resolution only happens as a
// direct result of Begin()/Complete()/MarkInterrupted()/Reconcile()
// calls, not on a timer.
class OperationLedger {
public:
    // Begins (or resumes reasoning about) the operation named `id`.
    // A brand-new id always returns Started and creates a record with
    // outcome=Unknown. An id whose existing record is Succeeded returns
    // AlreadyCompleted without touching the record. An id whose existing
    // record is still Unknown (finished_at == 0, i.e. an unresolved
    // prior attempt) returns NeedsReconciliation without touching the
    // record -- Reconcile() must be called first. An id whose existing
    // record is Failed is reset (kind overwritten with this call's
    // `kind`, started_at refreshed, finished_at back to 0, outcome back
    // to Unknown) and returns Started, since a definite failure is safe
    // to retry.
    Result<OperationBeginOutcome> Begin(const std::string& id, const std::string& kind);

    // Records a definite, directly-observed result. `outcome` must be
    // Succeeded or Failed (Unknown is rejected -- this call is for
    // reporting a confirmed result, not for un-confirming one).
    Result<void> Complete(const std::string& id, OperationOutcome outcome);

    // Records that `id` was interrupted, for the (comparatively rare)
    // case where the interruption itself was directly observed rather
    // than inferred later. Leaves outcome at Unknown (an observed
    // interruption is still not a known success/failure) but stamps
    // finished_at and `reason` so a later Reconcile() has more to go on.
    Result<void> MarkInterrupted(const std::string& id, const std::string& reason);

    // Resolves an Unknown-outcome record against reality: calls `probe`
    // (a caller-supplied check against actual state -- process
    // liveness, `git status`, an artifact's existence on disk; this
    // ledger deliberately has no built-in knowledge of any of those, the
    // same decoupling ResourceLeaseLedger keeps from SessionManager). If
    // `probe` reports Succeeded/Failed, the record is Complete()'d with
    // that outcome and it is returned. If `probe` itself reports
    // Unknown, the record is left untouched and Unknown is returned --
    // matching the design note's "一致しない場合は自動では進めずユーザー
    // へ提示する": this ledger does not guess on the caller's behalf.
    // Fails if `id` is not tracked, or if `probe` itself fails.
    Result<OperationOutcome> Reconcile(const std::string& id, const std::function<Result<OperationOutcome>()>& probe);

    [[nodiscard]] std::optional<OperationRecord> Find(const std::string& id) const;
    [[nodiscard]] std::vector<OperationRecord> All() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, OperationRecord> records_;
};

} // namespace aistudio::core
