#include "test_framework.hpp"
#include "Core/Context/ContextRestorer.hpp"
#include "Core/Security/Sandbox.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>

using namespace aistudio::core;

namespace {

namespace fs = std::filesystem;

struct TempProject {
    fs::path root;

    TempProject() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_context_restorer_test_" + std::to_string(counter++));
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
};

ContextSnapshot MakeSnapshotOf(std::vector<std::string> included_item_ids) {
    ContextSnapshot snapshot;
    snapshot.id = "snap.1";
    snapshot.task_name = "test";
    snapshot.included_item_ids = std::move(included_item_ids);
    return snapshot;
}

// Same as MakeSnapshotOf, but with an explicit, per-id ContextSourceKind
// -- what a real MakeContextSnapshot() call produces since the "Context
// Restore" source_kind work, and what ContextRestorer actually dispatches
// on (see its own class comment). `kinds` must be the same length as
// `ids`.
ContextSnapshot MakeSnapshotWithKinds(std::vector<std::string> ids, std::vector<ContextSourceKind> kinds) {
    ContextSnapshot snapshot = MakeSnapshotOf(std::move(ids));
    snapshot.included_item_source_kinds = std::move(kinds);
    return snapshot;
}

} // namespace

AISTUDIO_TEST(ContextRestorer_Restore_FileId_ReadsCurrentContent) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotOf({"a.hpp"}));

    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].id == "a.hpp");
    AISTUDIO_EXPECT(items[0].source == ContextSourceKind::File);
    AISTUDIO_EXPECT(items[0].content == "class Foo {};\n");
}

AISTUDIO_TEST(ContextRestorer_Restore_ReflectsCurrentContent_NotSnapshotTimeContent) {
    // ContextSnapshot stores only ids, never content (see its own class
    // comment) -- Restore() must read whatever the file says NOW, not
    // whatever it said when the snapshot was taken.
    TempProject project;
    project.WriteFile("a.hpp", "original\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto snapshot = MakeSnapshotOf({"a.hpp"});

    project.WriteFile("a.hpp", "edited since the snapshot\n");
    const auto items = restorer.Restore(snapshot);

    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].content == "edited since the snapshot\n");
}

AISTUDIO_TEST(ContextRestorer_Restore_DeletedFile_IsSilentlySkipped) {
    TempProject project;
    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});

    const auto items = restorer.Restore(MakeSnapshotOf({"never_existed.hpp"}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_NonFileShapedId_IsSilentlySkipped) {
    // A Symbol-source id ("path:name") never names an openable file path,
    // so it's naturally skipped without any explicit id-shape dispatch —
    // see the class comment.
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotOf({"a.hpp:Foo"}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_MixOfResolvableAndUnresolvable_KeepsOnlyResolvable) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");
    project.WriteFile("b.hpp", "class Bar {};\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotOf({"a.hpp", "deleted.hpp", "b.hpp"}));

    AISTUDIO_EXPECT(items.size() == 2);
}

AISTUDIO_TEST(ContextRestorer_Restore_FirewallBlocksDeniedPath) {
    TempProject project;
    // "*secret*" is one of Sandbox's default deny patterns.
    project.WriteFile("my_secret.hpp", "class Attack {};\n");

    const Sandbox firewall(project.RootString());
    const ContextRestorer restorer(
        ContextRestorer::Options{.project_root = project.RootString(), .firewall = &firewall});

    const auto items = restorer.Restore(MakeSnapshotOf({"my_secret.hpp"}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_FirewallAllowsOrdinaryPath) {
    TempProject project;
    project.WriteFile("a.hpp", "class Foo {};\n");

    const Sandbox firewall(project.RootString());
    const ContextRestorer restorer(
        ContextRestorer::Options{.project_root = project.RootString(), .firewall = &firewall});

    const auto items = restorer.Restore(MakeSnapshotOf({"a.hpp"}));
    AISTUDIO_EXPECT(items.size() == 1);
}

AISTUDIO_TEST(ContextRestorer_Restore_NoFirewall_AllowsEverythingReadable) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "class Attack {};\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotOf({"my_secret.hpp"}));
    AISTUDIO_EXPECT(items.size() == 1);
}

AISTUDIO_TEST(ContextRestorer_Restore_EmptySnapshot_ReturnsEmpty) {
    TempProject project;
    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    AISTUDIO_EXPECT(restorer.Restore(MakeSnapshotOf({})).empty());
}

// ---- Symbol-kind restoration ----

AISTUDIO_TEST(ContextRestorer_Restore_SymbolId_ResolvesViaSymbolIndex) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .symbol_index = &symbol_index});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"Player.hpp:PlayerAttack"}, {ContextSourceKind::Symbol}));

    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].id == "Player.hpp:PlayerAttack");
    AISTUDIO_EXPECT(items[0].source == ContextSourceKind::Symbol);
    AISTUDIO_EXPECT(items[0].content.find("PlayerAttack") != std::string::npos);
}

AISTUDIO_TEST(ContextRestorer_Restore_SymbolId_NoSymbolIndexConfigured_IsSkipped) {
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"Player.hpp:PlayerAttack"}, {ContextSourceKind::Symbol}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_SymbolId_SymbolNoLongerPresent_IsSilentlySkipped) {
    TempProject project;
    project.WriteFile("Player.hpp", "class SomethingElse {\n};\n"); // PlayerAttack removed since the snapshot

    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .symbol_index = &symbol_index});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"Player.hpp:PlayerAttack"}, {ContextSourceKind::Symbol}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_SymbolId_FirewallBlocksContainingFile) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "class Attack {\n};\n");

    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    const Sandbox firewall(project.RootString());

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .firewall = &firewall, .symbol_index = &symbol_index});
    const auto items = restorer.Restore(MakeSnapshotWithKinds({"my_secret.hpp:Attack"}, {ContextSourceKind::Symbol}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_SymbolId_OperatorArrowOverload_NotConfusedWithDependencyId) {
    // The whole reason ContextRestorer dispatches on a persisted
    // ContextSourceKind instead of guessing one from the id's string
    // shape: SymbolExtractor captures an operator overload's real name
    // (e.g. "operator->"), so this id contains BOTH a Symbol id's ':'
    // separator AND a Dependency id's "->" separator. A shape-only
    // dispatcher would have no correct way to tell these apart.
    TempProject project;
    project.WriteFile("Iter.hpp", "class Iter {\npublic:\n    int* operator->() { return p; }\nprivate:\n    int* p;\n};\n");

    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));

    // Confirm the extractor actually produced the ambiguous shape this
    // test exists to cover, rather than silently testing nothing.
    const auto matches = symbol_index.FindByName("operator->");
    AISTUDIO_EXPECT(matches.size() == 1);
    AISTUDIO_EXPECT(matches[0].file_path == "Iter.hpp");

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .symbol_index = &symbol_index});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"Iter.hpp:operator->"}, {ContextSourceKind::Symbol}));

    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].source == ContextSourceKind::Symbol);
    AISTUDIO_EXPECT(items[0].id == "Iter.hpp:operator->");
}

// ---- Dependency-kind restoration ----

AISTUDIO_TEST(ContextRestorer_Restore_DependencyId_ResolvesViaIncludeGraph) {
    TempProject project;
    project.WriteFile("b.hpp", "class B {};\n");
    project.WriteFile("a.hpp", "#include \"b.hpp\"\nclass A {};\n");

    IncludeGraph include_graph;
    AISTUDIO_EXPECT(include_graph.Build(project.RootString()));

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .include_graph = &include_graph});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"a.hpp->b.hpp"}, {ContextSourceKind::Dependency}));

    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].id == "a.hpp->b.hpp");
    AISTUDIO_EXPECT(items[0].source == ContextSourceKind::Dependency);
    AISTUDIO_EXPECT(items[0].content == "a.hpp includes b.hpp");
}

AISTUDIO_TEST(ContextRestorer_Restore_DependencyId_NoIncludeGraphConfigured_IsSkipped) {
    TempProject project;
    project.WriteFile("b.hpp", "class B {};\n");
    project.WriteFile("a.hpp", "#include \"b.hpp\"\nclass A {};\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"a.hpp->b.hpp"}, {ContextSourceKind::Dependency}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_DependencyId_EdgeNoLongerExists_IsSilentlySkipped) {
    TempProject project;
    project.WriteFile("b.hpp", "class B {};\n");
    project.WriteFile("a.hpp", "class A {};\n"); // no longer includes b.hpp

    IncludeGraph include_graph;
    AISTUDIO_EXPECT(include_graph.Build(project.RootString()));

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .include_graph = &include_graph});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"a.hpp->b.hpp"}, {ContextSourceKind::Dependency}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_DependencyId_FirewallBlocksEitherEndpoint) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "class B {};\n");
    project.WriteFile("a.hpp", "#include \"my_secret.hpp\"\nclass A {};\n");

    IncludeGraph include_graph;
    AISTUDIO_EXPECT(include_graph.Build(project.RootString()));
    const Sandbox firewall(project.RootString());

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .firewall = &firewall, .include_graph = &include_graph});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"a.hpp->my_secret.hpp"}, {ContextSourceKind::Dependency}));
    AISTUDIO_EXPECT(items.empty());
}

// ---- Keyword-shaped Custom-kind restoration ----

AISTUDIO_TEST(ContextRestorer_Restore_KeywordId_ResolvesCurrentLineContent) {
    TempProject project;
    project.WriteFile("a.cpp", "line one\nline two has KEYWORD here\nline three\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotWithKinds({"keyword:a.cpp:2"}, {ContextSourceKind::Custom}));

    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].id == "keyword:a.cpp:2");
    AISTUDIO_EXPECT(items[0].source == ContextSourceKind::Custom);
    AISTUDIO_EXPECT(items[0].content == "a.cpp:2: line two has KEYWORD here");
}

AISTUDIO_TEST(ContextRestorer_Restore_KeywordId_TrimsLineContentLikeKeywordSearchDoes) {
    TempProject project;
    project.WriteFile("a.cpp", "line one\n    indented KEYWORD line   \nline three\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotWithKinds({"keyword:a.cpp:2"}, {ContextSourceKind::Custom}));

    AISTUDIO_EXPECT(items.size() == 1);
    AISTUDIO_EXPECT(items[0].content == "a.cpp:2: indented KEYWORD line");
}

AISTUDIO_TEST(ContextRestorer_Restore_KeywordId_LineNoLongerExists_IsSilentlySkipped) {
    TempProject project;
    project.WriteFile("a.cpp", "only one line\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotWithKinds({"keyword:a.cpp:99"}, {ContextSourceKind::Custom}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_KeywordId_FirewallBlocksDeniedPath) {
    TempProject project;
    project.WriteFile("my_secret.hpp", "class Attack {\n};\n");

    const Sandbox firewall(project.RootString());
    const ContextRestorer restorer(
        ContextRestorer::Options{.project_root = project.RootString(), .firewall = &firewall});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"keyword:my_secret.hpp:1"}, {ContextSourceKind::Custom}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_KeywordId_MalformedLineNumber_IsSilentlySkipped) {
    TempProject project;
    project.WriteFile("a.cpp", "some content\n");

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"keyword:a.cpp:not-a-number"}, {ContextSourceKind::Custom}));
    AISTUDIO_EXPECT(items.empty());
}

AISTUDIO_TEST(ContextRestorer_Restore_CustomId_NotKeywordShaped_IsUnresolvableAndSkipped) {
    // e.g. ProjectRulesBackend's "rules/project" -- a Custom id with no
    // source this class can re-read.
    TempProject project;

    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items = restorer.Restore(MakeSnapshotWithKinds({"rules/project"}, {ContextSourceKind::Custom}));
    AISTUDIO_EXPECT(items.empty());
}

// ---- GitDiff-kind: no restoration source yet ----

AISTUDIO_TEST(ContextRestorer_Restore_GitDiffId_HasNoSourceYet_IsSkipped) {
    TempProject project;
    const ContextRestorer restorer(ContextRestorer::Options{.project_root = project.RootString()});
    const auto items =
        restorer.Restore(MakeSnapshotWithKinds({"git/commit/abc123"}, {ContextSourceKind::GitDiff}));
    AISTUDIO_EXPECT(items.empty());
}

// ---- Legacy snapshots (no recorded source kinds) ----

AISTUDIO_TEST(ContextRestorer_Restore_LegacySnapshotWithoutSourceKinds_NeverAttemptsSymbolRestoration) {
    // A snapshot saved before included_item_source_kinds existed has an
    // EMPTY kinds vector, not one full of File -- Restore() must fall
    // back to the original file-shape-only guess for every id in that
    // snapshot, even when a SymbolIndex is configured and the id would
    // actually resolve if its kind WERE known. Otherwise a pre-migration
    // row would restore differently depending on whether the caller
    // happens to pass a SymbolIndex/IncludeGraph today, which isn't
    // determined by anything the snapshot itself recorded.
    TempProject project;
    project.WriteFile("Player.hpp", "class PlayerAttack {\n};\n");

    SymbolIndex symbol_index;
    AISTUDIO_EXPECT(symbol_index.Build(project.RootString()));
    AISTUDIO_EXPECT(!symbol_index.FindByName("PlayerAttack").empty()); // sanity: it WOULD resolve if attempted

    const ContextRestorer restorer(ContextRestorer::Options{
        .project_root = project.RootString(), .symbol_index = &symbol_index});
    const auto items = restorer.Restore(MakeSnapshotOf({"Player.hpp:PlayerAttack"})); // no kinds recorded
    AISTUDIO_EXPECT(items.empty());
}
