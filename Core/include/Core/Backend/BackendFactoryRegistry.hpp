#pragma once

#include "Core/Backend/IBackend.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace aistudio::core {

using BackendFactory = std::function<std::shared_ptr<IBackend>()>;

// Lets a Backend register itself at static-init time — the same idiom
// the test framework's AISTUDIO_TEST macro uses (Core/tests/
// test_framework.hpp) — so bootstrap code can discover and instantiate
// every statically-linked Backend without hardcoding its name
// (docs/MASTER_SPEC.md #33 Backend Registry: "Backendを追加しただけで
// AI Development Studioが認識できる仕組み"). Uses a function-local
// static (Meyer's singleton), so it's safe against static-initialization
// order across translation units.
//
// Out-of-process / plugin discovery needs the Plugin Protocol
// (docs/ROADMAP.md, not yet started) and is out of scope here.
class BackendFactoryRegistry {
public:
    static BackendFactoryRegistry& Instance();

    void Register(std::string name, BackendFactory factory);

    // Instantiates one Backend from every registered factory.
    [[nodiscard]] std::vector<std::shared_ptr<IBackend>> CreateAll() const;

    [[nodiscard]] std::size_t FactoryCount() const { return factories_.size(); }

private:
    BackendFactoryRegistry() = default;

    std::vector<std::pair<std::string, BackendFactory>> factories_;
};

struct BackendFactoryRegistrar {
    BackendFactoryRegistrar(const std::string& name, BackendFactory factory) {
        BackendFactoryRegistry::Instance().Register(name, std::move(factory));
    }
};

} // namespace aistudio::core

// Registers ClassName's default constructor as a Backend factory under
// its own type name. Place at namespace scope in the .cpp that defines
// ClassName.
#define AISTUDIO_REGISTER_BACKEND(ClassName)                                                     \
    static ::aistudio::core::BackendFactoryRegistrar aistudio_backend_registrar_##ClassName(     \
        #ClassName, [] { return std::make_shared<ClassName>(); })
