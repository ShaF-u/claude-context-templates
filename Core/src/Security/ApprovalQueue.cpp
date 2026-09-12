#include "Core/Security/ApprovalQueue.hpp"

#include "Core/Protocol/RequestId.hpp"
#include "Core/Util/Time.hpp"

namespace aistudio::core {

namespace {
Error NotFoundError(const std::string& id) {
    return Error{
        .code = ErrorCode::NotFound,
        .message = "no approval request: " + id,
        .module = "Core.Security.ApprovalQueue",
    };
}
} // namespace

std::string ApprovalQueue::Submit(Command command) {
    ApprovalRequest request;
    request.id = RequestIdGenerator::Next();
    request.command = std::move(command);
    request.status = ApprovalStatus::Pending;
    request.created_at = CurrentUnixTimestamp();
    const auto id = request.id;
    requests_.emplace(id, std::move(request));
    return id;
}

std::vector<ApprovalRequest> ApprovalQueue::Pending() const {
    std::vector<ApprovalRequest> result;
    for (const auto& [id, request] : requests_) {
        if (request.status == ApprovalStatus::Pending) {
            result.push_back(request);
        }
    }
    return result;
}

std::optional<ApprovalRequest> ApprovalQueue::Find(const std::string& id) const {
    const auto it = requests_.find(id);
    if (it == requests_.end()) {
        return std::nullopt;
    }
    return it->second;
}

Result<void> ApprovalQueue::Approve(const std::string& id) {
    const auto it = requests_.find(id);
    if (it == requests_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.status != ApprovalStatus::Pending) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "approval request already resolved: " + id,
            .module = "Core.Security.ApprovalQueue",
        });
    }
    it->second.status = ApprovalStatus::Approved;
    return Result<void>::Ok();
}

Result<void> ApprovalQueue::Reject(const std::string& id) {
    const auto it = requests_.find(id);
    if (it == requests_.end()) {
        return Result<void>::Fail(NotFoundError(id));
    }
    if (it->second.status != ApprovalStatus::Pending) {
        return Result<void>::Fail(Error{
            .code = ErrorCode::InvalidArgument,
            .message = "approval request already resolved: " + id,
            .module = "Core.Security.ApprovalQueue",
        });
    }
    it->second.status = ApprovalStatus::Rejected;
    return Result<void>::Ok();
}

void ApprovalQueue::Remove(const std::string& id) {
    requests_.erase(id);
}

} // namespace aistudio::core
