#pragma once

// Tiny dependency-free test runner. Swap for GoogleTest/Catch2 once
// Dependency Management (docs/DEPENDENCY_MANAGEMENT.md) brings in a
// package manager; keeps Phase 0 free of network-fetched third-party code.

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace aistudio::testing {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        Registry().push_back({std::move(name), std::move(fn)});
    }
};

inline int RunAll() {
    int failures = 0;
    for (const auto& test : Registry()) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << test.name << ": " << e.what() << "\n";
            ++failures;
        }
    }
    std::cout << (failures == 0 ? "All tests passed.\n" : "Some tests failed.\n");
    return failures == 0 ? 0 : 1;
}

} // namespace aistudio::testing

#define AISTUDIO_TEST(name) \
    void name(); \
    static ::aistudio::testing::Registrar registrar_##name(#name, name); \
    void name()

#define AISTUDIO_EXPECT(cond) \
    if (!(cond)) throw std::runtime_error(std::string("expectation failed: ") + #cond)
