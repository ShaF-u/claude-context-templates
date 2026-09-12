cmake_minimum_required(VERSION 3.24)
project(AIStudioCore LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored third-party C dependency, added before our own warning flags
# below so it never compiles under /W4 / -Wall -Wextra -Wpedantic (see
# Core/vendor/sqlite3/CMakeLists.txt, docs/DEPENDENCY_MANAGEMENT.md).
include(FetchContent)
FetchContent_Declare(
    sqlite3
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA3_256=628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e
)
FetchContent_MakeAvailable(sqlite3)
add_subdirectory(vendor/sqlite3)

# Vendored parsing library for Core/Index's SymbolExtractor
# (docs/ROADMAP.md Phase 3 "Project Intelligence"). FetchContent names
# ("treesitter"/"treesittercpp") avoid the literal hyphens in the
# upstream project names, since CMake FetchContent variable prefixes are
# derived from this name and hyphens aren't valid there.
# SOURCE_SUBDIR points at a real subdirectory that has no CMakeLists.txt
# of its own ("lib" here). Upstream's own top-level CMakeLists.txt
# regenerates parser.c via the tree-sitter CLI, which isn't installed in
# this environment; pointing SOURCE_SUBDIR away from it stops
# FetchContent_MakeAvailable() from auto-add_subdirectory-ing it, while
# still fully populating treesitter_SOURCE_DIR so our own
# vendor/tree_sitter/CMakeLists.txt (added below) can compile lib.c
# directly, isolated from /W4 like every other vendored dependency.
FetchContent_Declare(
    treesitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.0.tar.gz
    URL_HASH SHA256=d35c96e68736bd9569d2757c3cc71052485f33082c3825f1aed9d0e86013a159
    SOURCE_SUBDIR lib
)
FetchContent_MakeAvailable(treesitter)
add_subdirectory(vendor/tree_sitter)

# Git, not the release tarball: tree-sitter-cpp's v0.23.4 release archive
# contains a duplicate "src/grammar.json" entry (once as "./src/..." and
# once as "src/...") that CMake's bundled libarchive extractor fails to
# overwrite on Windows. GIT_TAG is pinned to the tag's commit SHA (from
# `git ls-remote --tags`) for the same reproducibility a hash gives.
# SOURCE_SUBDIR "src" has the same purpose as treesitter's above: it has
# no CMakeLists.txt of its own, so upstream's real (CLI-dependent)
# top-level CMakeLists.txt is never auto-included.
FetchContent_Declare(
    treesittercpp
    GIT_REPOSITORY https://github.com/tree-sitter/tree-sitter-cpp.git
    GIT_TAG f41e1a044c8a84ea9fa8577fdd2eab92ec96de02 # v0.23.4
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR src
)
FetchContent_MakeAvailable(treesittercpp)
add_subdirectory(vendor/tree_sitter_cpp)

# Header-only vendored dependencies for the Application API layer
# (Core/API — docs/ROADMAP.md Phase 0 P1 "Core <-> Frontend communication
# design"). INTERFACE libraries have nothing of their own to compile, so
# unlike sqlite3 they don't need directory-scope isolation from /W4 —
# warnings from their headers are suppressed per-includer instead (see
# Core/src/API/ApiServer.cpp).
FetchContent_Declare(
    httplib
    URL https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.54.1/httplib.h
    URL_HASH SHA256=5933C14B2D0F45212925ED18CA579841F5FCE717F431FC20CEC712423E905B10
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(httplib)
add_library(httplib INTERFACE)
target_include_directories(httplib INTERFACE ${httplib_SOURCE_DIR})

FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.hpp
    URL_HASH SHA256=AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(nlohmann_json)
add_library(nlohmann_json INTERFACE)
target_include_directories(nlohmann_json INTERFACE ${nlohmann_json_SOURCE_DIR})

if (MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(aistudio_core STATIC
    src/Logging/Logger.cpp
    src/Config/Config.cpp
    src/Backend/IBackend.cpp
    src/Backend/BackendRegistry.cpp
    src/Backend/CapabilityVersion.cpp
    src/Backend/BackendFactoryRegistry.cpp
    src/Plugin/PluginRegistry.cpp
    src/Plugin/PluginLoader.cpp
    src/Plugin/PluginBackendAdapter.cpp
    src/Plugin/PluginPermissions.cpp
    src/Plugin/PluginBootstrap.cpp
    src/Event/EventBus.cpp
    src/Task/TaskQueue.cpp
    src/Task/TaskExecutor.cpp
    src/Protocol/RequestId.cpp
    src/Database/Database.cpp
    src/Database/Migrator.cpp
    src/Database/StringList.cpp
    src/Database/TaskRepository.cpp
    src/Database/ContextSnapshotRepository.cpp
    src/Database/ContextAuditRepository.cpp
    src/Database/SessionRepository.cpp
    src/Database/HandoffRepository.cpp
    src/Database/OperationLedgerRepository.cpp
    src/Database/ComparisonTrialRepository.cpp
    src/API/ApiServer.cpp
    src/MCP/McpServer.cpp
    src/LSP/LspServer.cpp
    src/IDE/EditorStateStore.cpp
    src/Project/FileScanner.cpp
    src/Project/FileCache.cpp
    src/Project/FileWatcher.cpp
    src/Context/ContextItem.cpp
    src/Context/ContextBudget.cpp
    src/Context/ContextCompressor.cpp
    src/Context/ContextSelector.cpp
    src/Context/FileContextSource.cpp
    src/Context/ContextSnapshot.cpp
    src/Context/ContextAuditEntry.cpp
    src/Context/SymbolContextSource.cpp
    src/Context/DependencyContextSource.cpp
    src/Context/AstContextSource.cpp
    src/Context/ContextRetriever.cpp
    src/Context/ContextRestorer.cpp
    src/Context/ContextGarbageCollector.cpp
    src/Context/ContextUsageSummary.cpp
    src/Context/EmbeddingCache.cpp
    src/Context/SummaryCache.cpp
    src/Context/ContextCache.cpp
    src/Context/SentLedger.cpp
    src/Context/FileProviderBackend.cpp
    src/Context/ProjectRulesBackend.cpp
    src/Index/Symbol.cpp
    src/Index/SymbolExtractor.cpp
    src/Index/SymbolIndex.cpp
    src/Index/AstExtractor.cpp
    src/Index/AstIndex.cpp
    src/Search/KeywordSearch.cpp
    src/Search/SymbolSearch.cpp
    src/LLM/ChatMessage.cpp
    src/LLM/EchoLLMProvider.cpp
    src/Index/IncludeExtractor.cpp
    src/Index/IncludeGraph.cpp
    src/Index/CallExtractor.cpp
    src/Index/CallGraph.cpp
    src/Index/InheritanceExtractor.cpp
    src/Index/InheritanceGraph.cpp
    src/Index/ReferenceExtractor.cpp
    src/Index/ReferenceGraph.cpp
    src/Analysis/ImpactAnalyzer.cpp
    src/Util/Time.cpp
    src/Util/Glob.cpp
    src/Util/Identifier.cpp
    src/Util/Utf16.cpp
    src/Util/Utf8.cpp
    src/Util/WorkspaceHash.cpp
    src/Util/TextRange.cpp
    src/Util/ProcessRunner.cpp
    src/Terminal/PtyProcess.cpp
    src/Session/Capability.cpp
    src/Session/Session.cpp
    src/Session/SessionManager.cpp
    src/Session/GenericCliAdapter.cpp
    src/Session/CliDiagnostics.cpp
    src/Session/OperationProbes.cpp
    src/Session/HandoffResumeOrchestrator.cpp
    src/Session/WorktreeMcpConfig.cpp
    src/Session/ResourceLeaseLedger.cpp
    src/Session/HandoffRecord.cpp
    src/Session/HandoffVerifier.cpp
    src/Session/OperationLedger.cpp
    src/Session/ComparisonBoard.cpp
    src/Security/PermissionPolicy.cpp
    src/Security/ApprovalQueue.cpp
    src/Security/Sandbox.cpp
    src/Git/GitBackend.cpp
)

target_include_directories(aistudio_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(aistudio_core PUBLIC sqlite3 httplib nlohmann_json tree_sitter tree_sitter_cpp)
if (WIN32)
    target_link_libraries(aistudio_core PUBLIC ws2_32)
endif()

add_executable(aistudio_core_cli src/main.cpp)
target_link_libraries(aistudio_core_cli PRIVATE aistudio_core)
if (MSVC)
    # aistudio_core is a static lib linked like a Unix archive: MSVC only
    # pulls in a .obj member that resolves a symbol some already-included
    # object references. A self-registering Backend (AISTUDIO_REGISTER_BACKEND,
    # e.g. FileProviderBackend) is referenced by nothing in main.cpp --
    # only its own static initializer's side effect matters -- so without
    # this its .obj (and registration) gets silently dropped from the
    # final exe. /WHOLEARCHIVE forces every aistudio_core member in,
    # keeping Backend Discovery's promise that adding a new native Backend
    # to the library needs no main.cpp change to take effect.
    target_link_options(aistudio_core_cli PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:aistudio_core>")
endif()

enable_testing()
add_subdirectory(tests)
