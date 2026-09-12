#include "test_framework.hpp"
#include "Core/Plugin/PluginLoader.hpp"

using namespace aistudio::core;

// PluginLoader only has a real (LoadLibraryW-based) implementation on
// Windows (see PluginLoader.hpp) -- these tests exercise it against a
// system DLL that's guaranteed present on any Windows machine rather than
// a purpose-built fixture DLL, keeping this test self-contained with no
// extra CMake build artifacts.
#if defined(_WIN32)

AISTUDIO_TEST(PluginLoader_Load_ValidLibrary_Succeeds) {
    PluginLoader loader;
    AISTUDIO_EXPECT(loader.Load("k32", L"kernel32.dll"));
    AISTUDIO_EXPECT(loader.IsLoaded("k32"));
}

AISTUDIO_TEST(PluginLoader_Load_NonexistentLibrary_Fails) {
    PluginLoader loader;
    const auto result = loader.Load("bad", L"this_dll_does_not_exist_aistudio_test.dll");
    AISTUDIO_EXPECT(result.IsError());
    AISTUDIO_EXPECT(!loader.IsLoaded("bad"));
}

AISTUDIO_TEST(PluginLoader_Load_DuplicateId_Fails) {
    PluginLoader loader;
    loader.Load("k32", L"kernel32.dll");
    AISTUDIO_EXPECT(loader.Load("k32", L"kernel32.dll").IsError());
}

AISTUDIO_TEST(PluginLoader_IsLoaded_UnknownId_ReturnsFalse) {
    PluginLoader loader;
    AISTUDIO_EXPECT(!loader.IsLoaded("does-not-exist"));
}

AISTUDIO_TEST(PluginLoader_ResolveSymbol_KnownExport_ReturnsNonNull) {
    PluginLoader loader;
    loader.Load("k32", L"kernel32.dll");
    AISTUDIO_EXPECT(loader.ResolveSymbol("k32", "GetCurrentProcessId") != nullptr);
}

AISTUDIO_TEST(PluginLoader_ResolveSymbol_UnknownExport_ReturnsNull) {
    PluginLoader loader;
    loader.Load("k32", L"kernel32.dll");
    AISTUDIO_EXPECT(loader.ResolveSymbol("k32", "ThisSymbolDoesNotExistAistudioTest") == nullptr);
}

AISTUDIO_TEST(PluginLoader_ResolveSymbol_NotLoaded_ReturnsNull) {
    PluginLoader loader;
    AISTUDIO_EXPECT(loader.ResolveSymbol("does-not-exist", "GetCurrentProcessId") == nullptr);
}

AISTUDIO_TEST(PluginLoader_Unload_RemovesFromLoaded) {
    PluginLoader loader;
    loader.Load("k32", L"kernel32.dll");
    loader.Unload("k32");
    AISTUDIO_EXPECT(!loader.IsLoaded("k32"));
}

AISTUDIO_TEST(PluginLoader_Unload_UnknownId_IsNoOp) {
    PluginLoader loader;
    loader.Unload("does-not-exist"); // must not crash
    AISTUDIO_EXPECT(!loader.IsLoaded("does-not-exist"));
}

AISTUDIO_TEST(PluginLoader_Load_AfterUnload_CanReload) {
    PluginLoader loader;
    loader.Load("k32", L"kernel32.dll");
    loader.Unload("k32");
    AISTUDIO_EXPECT(loader.Load("k32", L"kernel32.dll"));
    AISTUDIO_EXPECT(loader.IsLoaded("k32"));
}

#endif // defined(_WIN32)
