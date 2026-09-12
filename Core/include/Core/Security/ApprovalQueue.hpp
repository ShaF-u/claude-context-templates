#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Protocol/Command.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aistudio::core {

enum class ApprovalStatus { Pending, Approved, Rejected };

struct ApprovalRequest {
    std::string id;
    Command command;
    ApprovalStatus status = ApprovalStatus::Pending;
    std::int64_t created_at = 0;
};

// Holds Commands that PermissionPolicy flagged as needing human
// confirmation until someone calls Approve()/Reject() — e.g. via
// ApiServer's /api/approvals endpoints (docs/MASTER_SPEC.md #79 Remote
// Approval). In-memory only; approvals don't need to survive a restart.
class ApprovalQueue {
public:
    [[nodiscard]] std::string Submit(Command command);
    [[nodiscard]] std::vector<ApprovalRequest> Pending() const;
    [[nodiscard]] std::optional<ApprovalRequest> Find(const std::string& id) const;

    Result<void> Approve(const std::string& id);
    Result<void> Reject(const std::string& id);

    // Removes a resolved (Approved/Rejected) request, typically once its
    // Command has actually been dispatched, so Pending() stays clean.
    void Remove(const std::string& id);

private:
    std::unordered_map<std::string, ApprovalRequest> requests_;
};

} // namespace aistudio::core
