#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Session/OperationLedger.hpp"
#include "Core/Session/SessionManager.hpp"

#include <functional>
#include <string>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-7. 中断後の復旧と二重実行防止" flagged
// this as unimplemented: "実際にプロセス生存/git status/成果物の実在を
// 見るチェッカー自体...は未着手". This is that probe for the session
// case -- ready to pass to OperationLedger::Reconcile() when the
// operation being reconciled IS a session's own run (the caller
// registered an OperationLedger id equal to, or associated with, a
// SessionManager session id).
//
// Deliberately not provided here: a git-status-based probe or an
// artifact-existence probe. Unlike a session's exit code (an
// unambiguous Succeeded/Failed signal the CLI itself reported), "is the
// worktree clean" or "does this file exist" has no single honest mapping
// to Succeeded/Failed without knowing what the specific operation was
// supposed to produce -- building a generic one now would be guessing
// at a shape no real consumer has asked for yet (AGENT.md #14).
//
// Does not call SessionManager::PollAll() itself -- reflects whatever
// state `manager` already has as of the call. A caller that needs an
// up-to-date read should PollAll() first.
//
// LIFETIME: the returned function captures `manager` BY REFERENCE (it's
// meant to be handed straight to OperationLedger::Reconcile() and
// invoked later) -- `manager` must outlive every call to the returned
// function. Constructing a temporary SessionManager just to pass here
// (`SessionOutcomeProbe(SessionManager{}, id)`) leaves the closure
// holding a dangling reference the moment this call returns.
[[nodiscard]] std::function<Result<OperationOutcome>()> SessionOutcomeProbe(const SessionManager& manager,
                                                                             std::string session_id);

} // namespace aistudio::core
