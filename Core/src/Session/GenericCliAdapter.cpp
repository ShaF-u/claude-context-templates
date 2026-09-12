#include "Core/Session/GenericCliAdapter.hpp"

#include "Core/Util/Utf8.hpp"

namespace aistudio::core {

GenericCliAdapter::GenericCliAdapter(std::string name) : name_(std::move(name)) {}

const std::string& GenericCliAdapter::Name() const { return name_; }

const CliCapabilities& GenericCliAdapter::Capabilities() const { return capabilities_; }

bool GenericCliAdapter::Start(const CliProfile& profile) {
    return process_.Start(profile.command_line, 120, 32, profile.environment_variables,
                           Utf8ToWide(profile.working_directory));
}

void GenericCliAdapter::Stop() { process_.Stop(); }

bool GenericCliAdapter::IsRunning() { return process_.IsRunning(); }

std::optional<int> GenericCliAdapter::ExitCode() const { return process_.ExitCode(); }

const std::string& GenericCliAdapter::LastErrorMessage() const { return process_.LastErrorMessage(); }

void GenericCliAdapter::SendInput(std::string_view bytes) { process_.WriteInput(bytes); }

std::string GenericCliAdapter::TakeOutput() { return process_.TakeOutput(); }

void GenericCliAdapter::Resize(std::int16_t cols, std::int16_t rows) { process_.Resize(cols, rows); }

} // namespace aistudio::core
