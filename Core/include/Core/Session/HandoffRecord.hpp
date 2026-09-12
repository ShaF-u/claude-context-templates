#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aistudio::core {

// docs/ROADMAP.md Phase 13 "13-6. AI間の成果・引き継ぎ形式". Unknown is
// the honest default for a verification never actually run to
// completion -- never collapsed into Failed (that would make a
// timed-out/interrupted check indistinguishable from a real failure).
enum class HandoffVerificationOutcome { Unknown, Succeeded, Failed };

[[nodiscard]] std::string ToString(HandoffVerificationOutcome outcome);
[[nodiscard]] HandoffVerificationOutcome HandoffVerificationOutcomeFromString(const std::string& text);

// One test/verification command's claimed vs. actually-observed result.
// `claimed_summary` is free text the authoring AI wrote about what it
// believes happened -- informational only, never trusted as fact.
// `verified_outcome`/`verified_output`/`verified_at` are populated
// exclusively by RunAndVerifyCommand() (Core/Session/HandoffVerifier.hpp),
// which actually executes the command -- nothing in this struct lets an
// authoring AI set those fields directly to fake a Succeeded result
// (ROADMAP's "AIの自己申告と、実際に確認した検証結果を区別して記録する").
struct HandoffVerification {
    std::string command;
    std::string claimed_summary;
    HandoffVerificationOutcome verified_outcome = HandoffVerificationOutcome::Unknown;
    std::string verified_output;
    std::int64_t verified_at = 0;
};

// One AI-to-AI (or AI-to-user) handoff record. Deliberately separate from
// ContextSnapshot -- that stores "which ContextItems were selected for a
// task", this stores "what one session did with them and why" -- but a
// handoff references the snapshot it worked from (context_snapshot_id,
// empty if none) so a reader can reconstruct what that session actually
// saw when it made its claims.
struct HandoffRecord {
    std::string id;
    std::string session_id; // empty if not tied to a live Session
    std::string task_id;
    std::string base_commit_sha;
    std::string prerequisites;
    std::string change_summary;
    std::string rationale; // 判断理由
    std::string unresolved_items;
    std::string next_steps;
    std::string context_snapshot_id;
    std::vector<HandoffVerification> verifications;
    std::int64_t created_at = 0;
};

} // namespace aistudio::core
