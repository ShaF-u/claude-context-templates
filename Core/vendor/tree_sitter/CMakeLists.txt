# Vendored tree-sitter runtime, fetched by the parent CMakeLists.txt via
# FetchContent (pinned URL + SHA256, see docs/DEPENDENCY_MANAGEMENT.md).
# lib/src/lib.c is tree-sitter's own amalgamation (it #includes every
# other .c file in lib/src/ itself), so this is the only translation
# unit we need to compile. Kept in its own directory scope, added
# *before* the parent enables /W4, so this third-party C file is never
# compiled under our warning flags.
add_library(tree_sitter STATIC ${treesitter_SOURCE_DIR}/lib/src/lib.c)
target_include_directories(tree_sitter PUBLIC ${treesitter_SOURCE_DIR}/lib/include)

# lib.c uses _Static_assert (C11), which MSVC's default C dialect doesn't
# enable without an explicit /std flag.
set_target_properties(tree_sitter PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

if (MSVC)
    target_compile_options(tree_sitter PRIVATE /w)
else()
    target_compile_options(tree_sitter PRIVATE -w)
endif()
