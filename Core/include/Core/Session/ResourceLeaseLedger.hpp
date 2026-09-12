#pragma once

#include "Core/Error/Result.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-5. 共有リソースの実行調停" -- v1 scope is
// deliberately just a named-resource lease ledger (exclusive/shared
// locks + a FIFO wait queue against abstract resource names like "build
// output directory", "Unreal editor", "GPU"). This answers "may I run"
// -- it never runs anything itself. No build-execution subsystem exists
// in this codebase yet (confirmed by the earlier "Design Proposal: LSP
// Diagnostics" investigation); this ledger is the future consumer-
// agnostic piece that subsystem would call into once it exists.
enum class LeaseMode { Shared, Exclusive };

struct ResourceLeaseHolder {
    std::string session_id;
    LeaseMode mode = LeaseMode::Shared;
};

enum class LeaseAcquireOutcome { Acquired, Queued };

struct LeaseAcquireResult {
    LeaseAcquireOutcome outcome = LeaseAcquireOutcome::Acquired;
    // Non-empty only when outcome == Queued -- who currently holds the
    // resource, blocking this request (docs/ROADMAP.md 13-5's "待機理由
    // の表示" checklist item: a caller can show this directly).
    std::vector<ResourceLeaseHolder> blocking_holders;
};

// EventBus "ResourceLeaseAcquired" payload -- published whenever a
// previously-queued request gets promoted to holder (never for a request
// granted immediately by Acquire() itself; that caller already knows
// synchronously via its own return value).
struct ResourceLeaseAcquiredEvent {
    std::string resource_name;
    ResourceLeaseHolder holder;
};

// Synchronous, no background thread (same shape as TaskQueue/
// SessionManager) -- promotion of queued requests only happens as a
// direct result of Acquire()/Release()/ReleaseAllForSession() calls, not
// on a timer.
//
// Reclaiming a crashed/terminated session's leases is deliberately tied
// to SessionManager observing that session's actual process death
// (ReleaseAllForSession(), called from there), not a lease TTL/timeout
// (AGENT.md #7 -- a TTL would either fire too early against a slow-but-
// alive session, or leave a genuinely dead session's lease held for the
// TTL's full duration for no reason when the truth is already knowable).
//
// Deliberately out of scope: cross-resource deadlock detection (a
// session that holds resource A and waits on B while another holds B
// and waits on A) -- a real feature in its own right, not something a
// v1 lease ledger with no actual consumer yet needs to solve
// speculatively (AGENT.md #14). Lease upgrade (a Shared holder
// requesting Exclusive on the same resource) is also not supported --
// Acquire() rejects a session that already holds/is queued for a
// resource; release the Shared lease first, then Acquire() Exclusive.
class ResourceLeaseLedger {
public:
    // Grants immediately if compatible with current holders and the
    // wait queue is empty (multiple Shared holders coexist; Exclusive
    // requires no other holders at all) -- otherwise queues the request
    // FIFO and returns Queued. A new Shared request is queued (not
    // granted early) whenever the wait queue is already non-empty, even
    // if it would otherwise be compatible with current holders --
    // without this, a steady stream of Shared requests could starve an
    // Exclusive request waiting its turn.
    //
    // Fails (without acquiring or queueing) if `session_id` already
    // holds or is queued for `resource_name`.
    Result<LeaseAcquireResult> Acquire(const std::string& resource_name, const std::string& session_id, LeaseMode mode);

    // Releases `session_id`'s hold on `resource_name`, or cancels its
    // queued wait for it. Promotes newly-unblocked queued requests,
    // publishing "ResourceLeaseAcquired" (payload: ResourceLeaseHolder)
    // for each one promoted (AGENT.md #5). Fails if `session_id` neither
    // holds nor is queued for `resource_name`.
    Result<void> Release(const std::string& resource_name, const std::string& session_id);

    // Releases every lease/queued-request `session_id` holds or wants,
    // across every resource -- see the class comment on why this (not a
    // TTL) is how a crashed session's leases get reclaimed.
    void ReleaseAllForSession(const std::string& session_id);

    [[nodiscard]] std::vector<ResourceLeaseHolder> HoldersOf(const std::string& resource_name) const;
    // session_ids only, FIFO order (a queued request's mode is available
    // via the blocking_holders it got back from its own Acquire() call).
    [[nodiscard]] std::vector<std::string> WaitQueueFor(const std::string& resource_name) const;

private:
    struct ResourceState {
        std::vector<ResourceLeaseHolder> holders;
        std::vector<ResourceLeaseHolder> wait_queue;
    };

    // Promotes as many leading wait_queue entries as are now compatible
    // with `state.holders` -- called with mutex_ already held, appends
    // each promotion to `promoted` for the caller to publish once
    // outside the lock.
    void PromoteQueued(ResourceState& state, std::vector<ResourceLeaseHolder>& promoted);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ResourceState> resources_;
};

} // namespace aistudio::core
