# Test-only fixture Plugin DLLs (see PluginBackendFixture.c's own comment)
# -- compiled as C (not C++) to prove Core/Plugin/AbiV1.h's C ABI actually
# crosses a real DLL boundary, not just in-process C++ against a fake
# vtable. Neither target links aistudio_core or anything else from this
# repo; AbiV1.h is deliberately self-contained (see its own file comment),
# so only Core's include directory is needed.
# PROJECT_SOURCE_DIR, not CMAKE_SOURCE_DIR: the latter is the repo root
# (../../../../CMakeLists.txt's AIDevelopmentStudio project, which
# add_subdirectory(Core)'s this whole tree in) -- PROJECT_SOURCE_DIR
# tracks the nearest enclosing project(), AIStudioCore from Core/
# CMakeLists.txt, i.e. Core/ itself.
add_library(aistudio_plugin_fixture SHARED PluginBackendFixture.c)
target_include_directories(aistudio_plugin_fixture PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(aistudio_plugin_fixture PROPERTIES C_STANDARD 11)

add_library(aistudio_plugin_fixture_broken SHARED PluginBackendFixtureBroken.c)
target_include_directories(aistudio_plugin_fixture_broken PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(aistudio_plugin_fixture_broken PROPERTIES C_STANDARD 11)
