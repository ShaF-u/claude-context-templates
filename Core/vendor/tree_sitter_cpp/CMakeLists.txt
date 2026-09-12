# Vendored tree-sitter-cpp grammar, fetched by the parent CMakeLists.txt
# via FetchContent (pinned URL + SHA256, see
# docs/DEPENDENCY_MANAGEMENT.md). parser.c is machine-generated (~17MB of
# parse tables) and scanner.c is its hand-written external scanner; both
# only need their own bundled src/tree_sitter/parser.h, not tree-sitter's
# public API headers. Kept in its own directory scope, added *before* the
# parent enables /W4, so this third-party/generated code is never
# compiled under our warning flags.
add_library(tree_sitter_cpp STATIC
    ${treesittercpp_SOURCE_DIR}/src/parser.c
    ${treesittercpp_SOURCE_DIR}/src/scanner.c
)
target_include_directories(tree_sitter_cpp PUBLIC ${treesittercpp_SOURCE_DIR}/src)

if (MSVC)
    target_compile_options(tree_sitter_cpp PRIVATE /w)
else()
    target_compile_options(tree_sitter_cpp PRIVATE -w)
endif()
