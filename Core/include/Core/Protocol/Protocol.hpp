#pragma once

#include "Core/Error/Result.hpp"
#include "Core/Protocol/Command.hpp"
#include "Core/Protocol/Event.hpp"
#include "Core/Protocol/Query.hpp"
#include "Core/Protocol/RequestId.hpp"

#include <any>

namespace aistudio::core {

// Outcome of dispatching a Command or Query to a Backend. The payload is
// intentionally untyped (std::any) at this layer — Backends document what
// they put in it per Capability, the same way a JSON API documents its
// response shape per endpoint.
using CommandResult = Result<std::any>;
using QueryResult = Result<std::any>;

} // namespace aistudio::core
