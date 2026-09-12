#include "Core/Session/HandoffResumeOrchestrator.hpp"

#include "Core/Util/Time.hpp"

namespace aistudio::core {

namespace {

// Same normalization Gui/src/Terminal.cpp's own paste handling applies
// (NormalizeLineEndingsForPty) -- duplicated here rather than shared
// since that one is a GUI-side concern (ImGui clipboard paste) and this
// is a Core-side one (a synthesized multi-line prompt), and the two
// having independent copies costs 4 lines each. A trailing "\r" submits
// the prompt, matching this codebase's own Enter-key binding
// (Terminal.cpp's kBindings: ImGuiKey_Enter -> "\r").
std::string ToPtyInput(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 1);
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
            out.push_back('\r');
        }
        out.push_back(text[i]);
    }
    out.push_back('\r');
    return out;
}

void AppendField(std::string& out, const std::string& label, const std::string& value) {
    if (value.empty()) {
        return;
    }
    out += label;
    out += ": ";
    out += value;
    out += "\n";
}

} // namespace

std::string BuildHandoffPrompt(const HandoffRecord& record) {
    std::string prompt = "[引き継ぎ情報]\n";
    AppendField(prompt, "task_id", record.task_id);
    AppendField(prompt, "base_commit_sha", record.base_commit_sha);
    AppendField(prompt, "前提", record.prerequisites);
    AppendField(prompt, "変更内容", record.change_summary);
    AppendField(prompt, "判断理由", record.rationale);
    AppendField(prompt, "未解決事項", record.unresolved_items);
    AppendField(prompt, "次の作業", record.next_steps);
    if (!record.verifications.empty()) {
        prompt += "検証結果:\n";
        for (const auto& verification : record.verifications) {
            prompt += "  - " + verification.command + ": " + ToString(verification.verified_outcome) + "\n";
        }
    }
    return prompt;
}

HandoffResumeOrchestrator::HandoffResumeOrchestrator(Options options) : options_(options) {}

bool HandoffResumeOrchestrator::ShouldAutoResume(const CliCapabilities& capabilities, SessionState ended_state) const {
    if (!options_.auto_trigger_enabled) {
        return false;
    }
    if (ended_state != SessionState::Failed) {
        return false;
    }
    return capabilities.resume_conversation != SupportLevel::Supported;
}

Result<Session> HandoffResumeOrchestrator::ResumeViaHandoff(SessionManager& manager, const HandoffRecord& record,
                                                              Session new_session, const CliProfile& profile,
                                                              std::unique_ptr<ICliAdapter> new_adapter) {
    auto result = manager.CreateSession(std::move(new_session), profile, std::move(new_adapter));
    if (!result) {
        return result;
    }
    {
        std::lock_guard lock(mutex_);
        resume_generated_session_ids_.insert(result.Value().id);
    }
    if (ICliAdapter* adapter = manager.Adapter(result.Value().id); adapter != nullptr) {
        adapter->SendInput(ToPtyInput(BuildHandoffPrompt(record)));
    }
    return result;
}

std::vector<Result<Session>> HandoffResumeOrchestrator::ObservePollResult(
    SessionManager& manager, const std::vector<std::string>& changed_session_ids,
    const std::function<std::optional<HandoffRecord>(const std::string&)>& find_handoff,
    const std::function<std::unique_ptr<ICliAdapter>()>& make_adapter,
    const std::function<CliProfile(const Session&)>& make_profile) {
    std::vector<Result<Session>> results;
    for (const auto& id : changed_session_ids) {
        {
            std::lock_guard lock(mutex_);
            if (resume_generated_session_ids_.contains(id)) {
                continue; // already one hop into a resume chain -- stop here
            }
        }
        const auto ended_session = manager.Find(id);
        if (!ended_session.has_value()) {
            continue;
        }
        ICliAdapter* ended_adapter = manager.Adapter(id);
        if (ended_adapter == nullptr || !ShouldAutoResume(ended_adapter->Capabilities(), ended_session->state)) {
            continue;
        }
        const auto record = find_handoff(id);
        if (!record.has_value()) {
            continue;
        }

        Session new_session;
        new_session.id = id + "-resume-" + std::to_string(CurrentUnixTimestamp());
        new_session.project_id = ended_session->project_id;
        new_session.workspace_id = ended_session->workspace_id;
        new_session.task_id = ended_session->task_id;
        new_session.cli_name = ended_session->cli_name;

        results.push_back(
            ResumeViaHandoff(manager, *record, std::move(new_session), make_profile(*ended_session), make_adapter()));
    }
    return results;
}

} // namespace aistudio::core
