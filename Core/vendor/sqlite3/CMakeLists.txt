# Vendored SQLite3 amalgamation, fetched by the parent CMakeLists.txt via
# FetchContent (pinned URL + SHA3-256, see docs/DEPENDENCY_MANAGEMENT.md).
# Kept in its own directory scope, added *before* the parent enables
# /W4 (or -Wall -Wextra -Wpedantic), so this third-party C file is never
# compiled under our warning flags.
add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})

if (MSVC)
    target_compile_options(sqlite3 PRIVATE /w)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()
