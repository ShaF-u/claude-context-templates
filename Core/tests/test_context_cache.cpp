#include "test_framework.hpp"
#include "Core/Context/ContextCache.hpp"
#include "Core/IDE/EditorStateStore.hpp"
#include "Core/Index/SymbolIndex.hpp"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <json.hpp>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <optional>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

// Same shape as test_context_retriever.cpp's own TempStateFile.
struct TempStateFile {
    fs::path path;

    TempStateFile() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
               fs::path("aistudio_context_cache_editor_state_" + std::to_string(counter++) + ".json");
        fs::remove(path);
    }

    ~TempStateFile() { fs::remove(path); }

    void WriteActiveDocument(const std::string& active_document_path) const {
        std::ofstream out(path, std::ios::binary);
        out << Json{
            {"version", 1},
            {"source", "vscode"},
            {"active_document_path", active_document_path},
            {"selection", nullptr},
            {"updated_at", ""},
        }.dump();
    }

    [[nodiscard]] std::string PathString() const { return path.string(); }
};

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_context_cache_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
    }

    ~TempProject() { fs::remove_all(root); }

    void WriteFile(const std::string& relative_path, const std::string& content) const {
        const fs::path full_path = root / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
    }

    [[nodiscard]] std::string RootString() const { return root.string(); }
    [[nodiscard]] std::string Path(const std::string& relative_path) const { return (root / relative_path).string(); }
};

// nullopt if no item with this id exists.
std::optional<int> PriorityOf(const std::vector<ContextItem>& items, const std::string& id) {
    const auto it = std::find_if(items.begin(), items.end(), [&](const ContextItem& item) { return item.id == id; });
    if (it == items.end()) {
        return std::nullopt;
    }
    return it->priority;
}

} // namespace

AISTUDIO_TEST(ContextCache_GetOrRetrieve_SameIntentTwice_SecondCallIsCacheHit) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever retriever(options);

    ContextCache cache;
    const auto first = cache.GetOrRetrieve(retriever, "PlayerAttack");
    const auto second = cache.GetOrRetrieve(retriever, "PlayerAttack");

    AISTUDIO_EXPECT(!first.empty());
    AISTUDIO_EXPECT(first.size() == second.size());
    const auto stats = cache.Stats();
    AISTUDIO_EXPECT(stats.hits == 1);
    AISTUDIO_EXPECT(stats.misses == 1);
}

AISTUDIO_TEST(ContextCache_GetOrRetrieve_DifferentIntents_AreIndependentEntries) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");
    project.WriteFile("Weapon.hpp", "class Weapon {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever retriever(options);

    ContextCache cache;
    const auto a = cache.GetOrRetrieve(retriever, "PlayerAttack");
    const auto b = cache.GetOrRetrieve(retriever, "Weapon");

    AISTUDIO_EXPECT(!a.empty());
    AISTUDIO_EXPECT(!b.empty());
    AISTUDIO_EXPECT(cache.Stats().size == 2);
}

AISTUDIO_TEST(ContextCache_InvalidateAll_ForcesFreshRetrieveReflectingProjectChanges) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.project_root = project.RootString();
    const ContextRetriever retriever(options);

    ContextCache cache;
    const auto before = cache.GetOrRetrieve(retriever, "PlayerAttack");

    // A new file appears (e.g. a rename split it into two files) --
    // simulates the underlying project changing after the intent was
    // already cached. SymbolIndex isn't rebuilt here since this test is
    // specifically about ContextCache's own invalidation, not
    // SymbolIndex's incremental reload (covered elsewhere).
    project.WriteFile("PlayerAttackHelper.hpp", "// PlayerAttack helper\n");

    // Without invalidation, the stale cached answer would keep coming
    // back even though File retrieval (which re-scans the whole project
    // every call) would now also match the new file.
    const auto still_cached = cache.GetOrRetrieve(retriever, "PlayerAttack");
    AISTUDIO_EXPECT(before.size() == still_cached.size());

    cache.InvalidateAll();
    const auto after = cache.GetOrRetrieve(retriever, "PlayerAttack");
    AISTUDIO_EXPECT(after.size() > before.size());
}

AISTUDIO_TEST(ContextCache_Invalidate_RemovesOnlyThatIntent) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");
    project.WriteFile("Weapon.hpp", "class Weapon {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever retriever(options);

    ContextCache cache;
    const auto a1 = cache.GetOrRetrieve(retriever, "PlayerAttack");
    const auto b1 = cache.GetOrRetrieve(retriever, "Weapon");
    AISTUDIO_EXPECT(!a1.empty());
    AISTUDIO_EXPECT(!b1.empty());

    cache.Invalidate(retriever, "PlayerAttack");

    AISTUDIO_EXPECT(cache.Stats().size == 1);
    const auto b2 = cache.GetOrRetrieve(retriever, "Weapon");
    AISTUDIO_EXPECT(b2.size() == b1.size());
    // "Weapon" stayed cached (still a hit); only "PlayerAttack" needs a
    // fresh Retrieve() -- 2 initial misses + 1 hit for "Weapon" here.
    AISTUDIO_EXPECT(cache.Stats().hits == 1);
}

AISTUDIO_TEST(ContextCache_GetOrRetrieve_EmptyIntent_StillCachesEmptyResult) {
    const ContextRetriever retriever(ContextRetriever::Options{});
    ContextCache cache;

    const auto first = cache.GetOrRetrieve(retriever, "");
    const auto second = cache.GetOrRetrieve(retriever, "");

    AISTUDIO_EXPECT(first.empty());
    AISTUDIO_EXPECT(second.empty());
    AISTUDIO_EXPECT(cache.Stats().hits == 1);
}

// docs/ROADMAP.md CE-4: reproduces the "code review, 2026-09-05" known
// limitation the old intent-only cache key had -- before the fix, this
// second GetOrRetrieve() would wrongly return retriever_a's cached hit.
AISTUDIO_TEST(ContextCache_GetOrRetrieve_DifferentRetrievers_DoNotShareEntries) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index_a;
    symbol_index_a.Build(project.RootString());
    ContextRetriever::Options options_a;
    options_a.symbol_index = &symbol_index_a;
    const ContextRetriever retriever_a(options_a);

    SymbolIndex symbol_index_b; // never Build()'d -- always empty
    ContextRetriever::Options options_b;
    options_b.symbol_index = &symbol_index_b;
    const ContextRetriever retriever_b(options_b);

    ContextCache cache;
    const auto from_a = cache.GetOrRetrieve(retriever_a, "PlayerAttack");
    const auto from_b = cache.GetOrRetrieve(retriever_b, "PlayerAttack");

    AISTUDIO_EXPECT(!from_a.empty());
    AISTUDIO_EXPECT(from_b.empty());
}

// docs/ROADMAP.md CE-4: before the fix, this second GetOrRetrieve() call
// (same retriever, same intent) would return the first call's cached,
// now-stale priority instead of reflecting the new active document.
AISTUDIO_TEST(ContextCache_GetOrRetrieve_ActiveDocumentChanges_IsNotServedStale) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    TempStateFile state_file; // not written yet -- no active document
    const EditorStateStore editor_state_store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
    });

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.editor_state_store = &editor_state_store;
    const ContextRetriever retriever(options);

    ContextCache cache;
    const auto before = cache.GetOrRetrieve(retriever, "PlayerAttack");
    const auto before_priority = PriorityOf(before, "Player.hpp:PlayerAttack");
    AISTUDIO_EXPECT(before_priority.has_value());

    state_file.WriteActiveDocument(project.Path("Player.hpp"));
    const auto after = cache.GetOrRetrieve(retriever, "PlayerAttack");
    const auto after_priority = PriorityOf(after, "Player.hpp:PlayerAttack");
    AISTUDIO_EXPECT(after_priority.has_value());

    AISTUDIO_EXPECT(*after_priority > *before_priority);
}
