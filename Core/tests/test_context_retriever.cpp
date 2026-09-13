#include "test_framework.hpp"
#include "Core/Backend/BackendRegistry.hpp"
#include "Core/Context/ContextRetriever.hpp"
#include "Core/IDE/EditorStateStore.hpp"
#include "Core/Plugin/PluginBackendAdapter.hpp"
#include "Core/Security/Sandbox.hpp"

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

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_context_retriever_test_" + std::to_string(counter++));
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

// Same shape as test_editor_state_store.cpp's own TempStateFile -- a
// state-file path unique per test case so parallel/sequential test runs
// never race on the one fixed path this class reads in real use.
struct TempStateFile {
    fs::path path;

    TempStateFile() {
        static std::atomic<int> counter{0};
        path = fs::temp_directory_path() /
               fs::path("aistudio_context_retriever_editor_state_" + std::to_string(counter++) + ".json");
        fs::remove(path);
    }

    ~TempStateFile() { fs::remove(path); }

    // Writes a well-formed EditorStateStore v1 document reporting
    // `active_document_path` as the active document with no selection --
    // Retrieve()'s Active File Bias only ever reads active_document_path,
    // so selection/source/updated_at aren't exercised by these tests.
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

bool HasSource(const std::vector<ContextItem>& items, ContextSourceKind kind) {
    return std::any_of(items.begin(), items.end(), [&](const ContextItem& item) { return item.source == kind; });
}

bool HasId(const std::vector<ContextItem>& items, const std::string& id) {
    return std::any_of(items.begin(), items.end(), [&](const ContextItem& item) { return item.id == id; });
}

// nullopt if no item with this id exists -- callers that expect one to
// exist should also assert HasId() first for a clearer failure.
std::optional<int> PriorityOf(const std::vector<ContextItem>& items, const std::string& id) {
    const auto it = std::find_if(items.begin(), items.end(), [&](const ContextItem& item) { return item.id == id; });
    if (it == items.end()) {
        return std::nullopt;
    }
    return it->priority;
}

} // namespace

AISTUDIO_TEST(ContextRetriever_Retrieve_EmptyIntent_ReturnsEmpty) {
    const ContextRetriever retriever(ContextRetriever::Options{});
    AISTUDIO_EXPECT(retriever.Retrieve("").empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_NoSourcesConfigured_ReturnsEmpty) {
    const ContextRetriever retriever(ContextRetriever::Options{});
    AISTUDIO_EXPECT(retriever.Retrieve("Attack").empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_MatchesSymbolByName) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("PlayerAttack");
    AISTUDIO_EXPECT(!items.empty());
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::Symbol));
    AISTUDIO_EXPECT(items.front().priority >= 35 && items.front().priority <= 85);
}

AISTUDIO_TEST(ContextRetriever_Retrieve_IncludesDependenciesOfMatchedSymbolFile) {
    TempProject project;
    project.WriteFile("Weapon.hpp", "class Weapon {\n};\n");
    project.WriteFile("Player.hpp", "#include \"Weapon.hpp\"\n\nclass PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    IncludeGraph include_graph;
    include_graph.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.include_graph = &include_graph;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("PlayerAttack");
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::Dependency));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_MatchesFileByPath) {
    TempProject project;
    project.WriteFile("CsvParser.cpp", "void Foo() {\n}\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.project_root = project.RootString();
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("CsvParser");
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::File));
    AISTUDIO_EXPECT(HasId(items, "CsvParser.cpp"));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_FallsBackToKeywordMatch) {
    TempProject project;
    project.WriteFile("a.cpp", "// implements combat strategy logic\nvoid Foo() {}\n");

    ContextRetriever::Options options;
    options.project_root = project.RootString();
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("strategy");
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::Custom));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_SuppressesCoarseHitsForFileAlreadyMatchedBySymbol) {
    TempProject project;
    project.WriteFile("Attack.hpp", "// attack helper\nclass Attack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.project_root = project.RootString();
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("attack");
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::Symbol));
    AISTUDIO_EXPECT(!HasId(items, "Attack.hpp"));
    AISTUDIO_EXPECT(!HasSource(items, ContextSourceKind::Custom));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_RespectsMaxSymbolMatches) {
    TempProject project;
    project.WriteFile("a.hpp", "class Attack1 {\n};\nclass Attack2 {\n};\nclass Attack3 {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.max_symbol_matches = 1;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("Attack");
    std::size_t symbol_count = 0;
    for (const auto& item : items) {
        if (item.source == ContextSourceKind::Symbol) {
            ++symbol_count;
        }
    }
    AISTUDIO_EXPECT(symbol_count == 1);
}

AISTUDIO_TEST(ContextRetriever_Retrieve_FirewallBlocksSymbolMatch) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns; a .hpp
    // extension keeps SymbolIndex indexing it (unlike a dotfile such as
    // ".env", which has no recognized source extension).
    project.WriteFile("my_secret.hpp", "class Attack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const Sandbox firewall(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.firewall = &firewall;
    const ContextRetriever retriever(options);

    AISTUDIO_EXPECT(retriever.Retrieve("Attack").empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_FirewallBlocksDependencyEdgeNamingFirewalledFile) {
    // A Dependency item used to still name a firewalled related_path by
    // path even though its content was never exposed (see
    // ContextRetriever.hpp's Context Firewall comment) -- "*secret*" is
    // one of Sandbox's default deny patterns.
    TempProject project;
    project.WriteFile("my_secret.hpp", "class Weapon {\n};\n");
    project.WriteFile("Player.hpp", "#include \"my_secret.hpp\"\n\nclass PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());
    IncludeGraph include_graph;
    include_graph.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.include_graph = &include_graph;
    const ContextRetriever unfirewalled(options);
    AISTUDIO_EXPECT(HasId(unfirewalled.Retrieve("PlayerAttack"), "Player.hpp->my_secret.hpp"));

    const Sandbox firewall(project.RootString());
    options.firewall = &firewall;
    const ContextRetriever firewalled(options);
    AISTUDIO_EXPECT(!HasId(firewalled.Retrieve("PlayerAttack"), "Player.hpp->my_secret.hpp"));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_FirewallBlocksFileMatch) {
    TempProject project;
    project.WriteFile(".env", "SOME_TOKEN=xyz\n");

    ContextRetriever::Options options;
    options.project_root = project.RootString();
    const ContextRetriever unfirewalled(options);
    AISTUDIO_EXPECT(HasSource(unfirewalled.Retrieve(".env"), ContextSourceKind::File));

    const Sandbox firewall(project.RootString());
    options.firewall = &firewall;
    const ContextRetriever firewalled(options);
    AISTUDIO_EXPECT(!HasSource(firewalled.Retrieve(".env"), ContextSourceKind::File));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_FirewallBlocksKeywordMatch) {
    TempProject project;
    project.WriteFile(".env", "// strategy notes for combat balancing\n");

    const Sandbox firewall(project.RootString());

    ContextRetriever::Options options;
    options.project_root = project.RootString();
    options.firewall = &firewall;
    const ContextRetriever retriever(options);

    AISTUDIO_EXPECT(retriever.Retrieve("strategy").empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_NoFirewall_AllowsEverything) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever retriever(options); // firewall left at nullptr

    AISTUDIO_EXPECT(!retriever.Retrieve("PlayerAttack").empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_MultiWordIntent_MatchesSymbolByEmbeddedIdentifier) {
    // Reproduces the real-world usage shape (docs example: "player attack
    // logic") that a literal whole-intent substring match against a
    // short identifier essentially never satisfies.
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("show me the PlayerAttack combat logic");
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::Symbol));
    AISTUDIO_EXPECT(HasId(items, "Player.hpp:PlayerAttack"));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_MultiWordIntent_MatchesFileByEmbeddedIdentifier) {
    TempProject project;
    project.WriteFile("CsvParser.cpp", "void Foo() {\n}\n");

    ContextRetriever::Options options;
    options.project_root = project.RootString();
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("how does the CsvParser file work");
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::File));
    AISTUDIO_EXPECT(HasId(items, "CsvParser.cpp"));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_MultiWordIntent_FallsBackToKeywordMatch) {
    TempProject project;
    project.WriteFile("a.cpp", "// implements combat strategy logic\nvoid Foo() {}\n");

    ContextRetriever::Options options;
    options.project_root = project.RootString();
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("what is the combat strategy here");
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::Custom));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_IntentWithNoSpaces_StillMatchesEmbeddedIdentifier) {
    // A sentence with no ASCII spaces at all (e.g. Japanese prose with an
    // English identifier stitched in, no delimiter between them) should
    // still tokenize the embedded identifier out -- every non-word byte,
    // including a multi-byte UTF-8 continuation byte, acts as a
    // delimiter (see TokenizeIntent's own comment).
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("これはPlayerAttackの説明です");
    AISTUDIO_EXPECT(HasId(items, "Player.hpp:PlayerAttack"));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_NoMatches_ReturnsEmpty) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    options.project_root = project.RootString();
    const ContextRetriever retriever(options);

    AISTUDIO_EXPECT(retriever.Retrieve("DoesNotExistAnywhere").empty());
}

// Below: Active File Bias (ContextRetriever.hpp class comment /
// Options::editor_state_store) -- an EditorStateStore-reported active
// document biasing Symbol/Dependency/File/Keyword item priority toward
// docs/MASTER_SPEC.md #14's "100 = 現在の編集対象" ceiling.

AISTUDIO_TEST(ContextRetriever_Retrieve_EditorStateStoreSetButNoStateFile_BehavesUnchanged) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    TempStateFile state_file; // never written -- EditorStateStore::Read() returns nullopt
    const EditorStateStore store(EditorStateStore::Options{.state_file_path = state_file.PathString()});

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever without_store(options);
    options.editor_state_store = &store;
    const ContextRetriever with_unset_store(options);

    const auto expected = without_store.Retrieve("PlayerAttack");
    const auto actual = with_unset_store.Retrieve("PlayerAttack");
    AISTUDIO_EXPECT(!expected.empty());
    AISTUDIO_EXPECT(expected.size() == actual.size());
    AISTUDIO_EXPECT(PriorityOf(expected, "Player.hpp:PlayerAttack") == PriorityOf(actual, "Player.hpp:PlayerAttack"));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_ActiveFileBias_BoostsMatchedSymbolInActiveDocument) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    TempStateFile state_file;
    state_file.WriteActiveDocument(project.Path("Player.hpp"));
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
    });

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever unbiased(options);
    options.editor_state_store = &store;
    const ContextRetriever biased(options);

    const auto unbiased_items = unbiased.Retrieve("PlayerAttack");
    const auto biased_items = biased.Retrieve("PlayerAttack");
    const auto unbiased_priority = PriorityOf(unbiased_items, "Player.hpp:PlayerAttack");
    const auto biased_priority = PriorityOf(biased_items, "Player.hpp:PlayerAttack");
    AISTUDIO_EXPECT(unbiased_priority.has_value());
    AISTUDIO_EXPECT(biased_priority.has_value());
    AISTUDIO_EXPECT(*biased_priority > *unbiased_priority);
    AISTUDIO_EXPECT(*biased_priority == 100); // 85 (max Symbol tier) + 20 active-file bias, clamped to the ceiling
}

AISTUDIO_TEST(ContextRetriever_Retrieve_ActiveFileBias_DoesNotBoostSymbolInDifferentFile) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");
    project.WriteFile("Other.hpp", "// unrelated\n");

    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    TempStateFile state_file;
    state_file.WriteActiveDocument(project.Path("Other.hpp")); // NOT the file the symbol match lives in
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
    });

    ContextRetriever::Options options;
    options.symbol_index = &symbol_index;
    const ContextRetriever unbiased(options);
    options.editor_state_store = &store;
    const ContextRetriever biased(options);

    const auto unbiased_priority = PriorityOf(unbiased.Retrieve("PlayerAttack"), "Player.hpp:PlayerAttack");
    const auto biased_priority = PriorityOf(biased.Retrieve("PlayerAttack"), "Player.hpp:PlayerAttack");
    AISTUDIO_EXPECT(unbiased_priority.has_value());
    AISTUDIO_EXPECT(biased_priority.has_value());
    AISTUDIO_EXPECT(*biased_priority == *unbiased_priority); // active document is a different, unrelated file
}

AISTUDIO_TEST(ContextRetriever_Retrieve_ActiveFileBias_BoostsFileIncludedByActiveDocument) {
    TempProject project;
    project.WriteFile("Weapon.hpp", "class Weapon {\n};\n");
    project.WriteFile("Player.hpp", "#include \"Weapon.hpp\"\n\nclass PlayerAttack {\n};\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());

    TempStateFile state_file;
    state_file.WriteActiveDocument(project.Path("Player.hpp")); // includes Weapon.hpp
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
    });

    ContextRetriever::Options options;
    options.include_graph = &include_graph;
    options.project_root = project.RootString();
    const ContextRetriever unbiased(options);
    options.editor_state_store = &store;
    const ContextRetriever biased(options);

    // Intent matches Weapon.hpp by filename only (no SymbolIndex configured
    // here), so this exercises the File-retrieval path with a "neighbor"
    // (IncludeGraph-adjacent, not the active document itself) bias.
    const auto unbiased_priority = PriorityOf(unbiased.Retrieve("Weapon"), "Weapon.hpp");
    const auto biased_priority = PriorityOf(biased.Retrieve("Weapon"), "Weapon.hpp");
    AISTUDIO_EXPECT(unbiased_priority.has_value());
    AISTUDIO_EXPECT(biased_priority.has_value());
    AISTUDIO_EXPECT(*biased_priority > *unbiased_priority);
    AISTUDIO_EXPECT(*biased_priority == *unbiased_priority + 10); // neighbor bias, half of the exact-match bias
}

AISTUDIO_TEST(ContextRetriever_Retrieve_ActiveFileBias_BoostsKeywordMatchInActiveDocument) {
    TempProject project;
    project.WriteFile("Player.hpp", "// implements combat strategy logic\nclass Foo {\n};\n");

    TempStateFile state_file;
    state_file.WriteActiveDocument(project.Path("Player.hpp"));
    const EditorStateStore store(EditorStateStore::Options{
        .state_file_path = state_file.PathString(),
        .project_root = project.RootString(),
    });

    ContextRetriever::Options options;
    options.project_root = project.RootString();
    const ContextRetriever unbiased(options);
    options.editor_state_store = &store;
    const ContextRetriever biased(options);

    const auto unbiased_items = unbiased.Retrieve("strategy");
    const auto biased_items = biased.Retrieve("strategy");
    AISTUDIO_EXPECT(HasSource(unbiased_items, ContextSourceKind::Custom));
    AISTUDIO_EXPECT(HasSource(biased_items, ContextSourceKind::Custom));
    // Keyword item ids embed the line number ("keyword:<path>:<line>"), so
    // find both by source kind instead of a known id.
    const auto find_keyword_priority = [](const std::vector<ContextItem>& items) {
        const auto it = std::find_if(items.begin(), items.end(),
                                      [](const ContextItem& item) { return item.source == ContextSourceKind::Custom; });
        return it->priority;
    };
    AISTUDIO_EXPECT(find_keyword_priority(biased_items) > find_keyword_priority(unbiased_items));
}

// Below: real DLL boundary coverage for Backend Context Provider
// retrieval (docs/MASTER_SPEC.md #71) -- loads the same
// aistudio_plugin_fixture used by test_plugin_backend_adapter.cpp
// through the actual PluginLoader -> LoadPluginBackend ->
// BackendRegistry -> ContextRetriever chain, not a mock IBackend.

namespace {

std::shared_ptr<IBackend> LoadFixtureBackend(PluginLoader& loader) {
    // Test-only, ASCII-range build paths only (see PluginLoader.cpp's own
    // MultiByteToWideChar-based conversion for the general case).
    const std::string narrow_path(AISTUDIO_PLUGIN_FIXTURE_PATH);
    loader.Load("fixture", std::wstring(narrow_path.begin(), narrow_path.end()));
    auto result = LoadPluginBackend(loader, "fixture");
    return std::shared_ptr<IBackend>(std::move(result.Value()));
}

} // namespace

AISTUDIO_TEST(ContextRetriever_Retrieve_BackendRegistryUnset_SkipsBackendProviderRetrieval) {
    PluginLoader loader;
    BackendRegistry registry;
    registry.Register(LoadFixtureBackend(loader));

    ContextRetriever::Options options; // backend_registry left at nullptr
    const ContextRetriever retriever(options);

    AISTUDIO_EXPECT(retriever.Retrieve("fixture_context").empty());
}

AISTUDIO_TEST(ContextRetriever_Retrieve_BackendRegistrySet_IncludesRealPluginItem) {
    PluginLoader loader;
    BackendRegistry registry;
    registry.Register(LoadFixtureBackend(loader));

    ContextRetriever::Options options;
    options.backend_registry = &registry;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("please match fixture_context here");
    AISTUDIO_EXPECT(HasId(items, "fixture:item1"));
    AISTUDIO_EXPECT(HasSource(items, ContextSourceKind::Custom));
}

AISTUDIO_TEST(ContextRetriever_Retrieve_BackendProvidedItem_FirewallBlocksSecretLikeId) {
    PluginLoader loader;
    BackendRegistry registry;
    registry.Register(LoadFixtureBackend(loader));

    TempProject project; // firewall needs some root, unrelated to the plugin's own id
    const Sandbox firewall(project.RootString());

    ContextRetriever::Options options;
    options.backend_registry = &registry;
    options.firewall = &firewall;
    const ContextRetriever retriever(options);

    const auto items = retriever.Retrieve("please match fixture_secret here");
    AISTUDIO_EXPECT(!HasId(items, "my_secret.txt")); // "*secret*" is a Sandbox default deny pattern
}

AISTUDIO_TEST(ContextRetriever_Retrieve_BackendProvidesNothingForThisIntent_NoItemsAdded) {
    PluginLoader loader;
    BackendRegistry registry;
    registry.Register(LoadFixtureBackend(loader));

    ContextRetriever::Options options;
    options.backend_registry = &registry;
    const ContextRetriever retriever(options);

    AISTUDIO_EXPECT(retriever.Retrieve("nothing the fixture recognizes").empty());
}
