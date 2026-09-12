# Test-only fixture executable (see ArgvEcho.cpp's own comment) -- a
# plain console app, not a DLL, so no ABI concerns; built as ordinary C++
# like the rest of Core rather than C-only (unlike plugin_backend_fixture,
# which is C-only specifically to prove Plugin ABI v1's C ABI crosses a
# real DLL boundary -- not applicable here).
add_executable(aistudio_argv_echo_fixture ArgvEcho.cpp)
