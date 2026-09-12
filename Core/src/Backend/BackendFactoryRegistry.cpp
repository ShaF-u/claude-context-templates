#include "Core/Backend/BackendFactoryRegistry.hpp"

namespace aistudio::core {

BackendFactoryRegistry& BackendFactoryRegistry::Instance() {
    static BackendFactoryRegistry instance;
    return instance;
}

void BackendFactoryRegistry::Register(std::string name, BackendFactory factory) {
    factories_.emplace_back(std::move(name), std::move(factory));
}

std::vector<std::shared_ptr<IBackend>> BackendFactoryRegistry::CreateAll() const {
    std::vector<std::shared_ptr<IBackend>> backends;
    backends.reserve(factories_.size());
    for (const auto& [name, factory] : factories_) {
        backends.push_back(factory());
    }
    return backends;
}

} // namespace aistudio::core
