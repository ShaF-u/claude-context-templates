#include "test_framework.hpp"
#include "Core/Analysis/ImpactAnalyzer.hpp"
#include "Core/Git/GitBackend.hpp"
#include "Core/Util/ProcessRunner.hpp"

#include <algorithm>
#include <any>
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
        root = fs::temp_directory_path() / fs::path("aistudio_impact_test_" + std::to_string(counter++));
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

bool Contains(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

const AffectedSymbol* FindSymbol(const std::vector<AffectedSymbol>& symbols, const std::string& name) {
    const auto it = std::find_if(symbols.begin(), symbols.end(),
                                  [&](const AffectedSymbol& s) { return s.symbol_name == name; });
    return it == symbols.end() ? nullptr : &*it;
}

const ChangedSymbol* FindChangedSymbol(const std::vector<ChangedSymbol>& symbols, const std::string& name) {
    const auto it = std::find_if(symbols.begin(), symbols.end(),
                                  [&](const ChangedSymbol& s) { return s.symbol_name == name; });
    return it == symbols.end() ? nullptr : &*it;
}

// AnalyzeChanges() needs a real `git diff` -- a TempProject with git
// plumbing on top (same shape as test_git_backend.cpp's TempGitRepo,
// duplicated here per this codebase's per-file test-helper convention).
struct TempGitRepo {
    fs::path root;

    TempGitRepo() {
        static std::atomic<int> counter{0};
        root = fs::temp_directory_path() / fs::path("aistudio_impact_git_test_" + std::to_string(counter++));
        fs::remove_all(root);
        fs::create_directories(root);
        RunGit({"init"});
        RunGit({"config", "user.email", "test@example.com"});
        RunGit({"config", "user.name", "Test"});
    }

    ~TempGitRepo() { fs::remove_all(root); }

    void WriteFile(const std::string& relative_path, const std::string& content) const {
        const fs::path full_path = root / relative_path;
        fs::create_directories(full_path.parent_path());
        std::ofstream out(full_path, std::ios::binary);
        out << content;
    }

    void RunGit(const std::vector<std::string>& args) const { (void)RunProcess("git", args, root.string()); }

    void Commit(const std::string& message) const {
        RunGit({"add", "-A"});
        RunGit({"commit", "-m", message});
    }

    [[nodiscard]] std::string RootString() const { return root.string(); }

    // The real `git diff` (unstaged, working tree vs index) for this repo
    // right now, via GitBackend's own "git.diff" Query -- deliberately
    // not a hand-crafted diff string, to test against whatever this
    // machine's actual git produces rather than an assumed format.
    [[nodiscard]] std::string Diff() const { return RunDiffQuery("git.diff"); }

    // "git.diff.head" (working tree vs HEAD) -- staged and unstaged
    // changes combined, the one whose new-side line numbers always match
    // the actual current file content (see GitBackend.hpp's own comment
    // on Handle() for why AnalyzeChanges() should use this one, not
    // "git.diff.staged" alone, when staged changes matter).
    [[nodiscard]] std::string DiffHead() const { return RunDiffQuery("git.diff.head"); }

    [[nodiscard]] std::string RunDiffQuery(const std::string& query_name) const {
        GitBackend backend;
        Config config;
        config.Set("project.root", RootString());
        backend.Configure(config);
        backend.Start();
        Query query;
        query.name = query_name;
        const auto result = backend.Handle(query);
        return result.IsOk() ? std::any_cast<std::string>(result.Value()) : std::string();
    }
};

} // namespace

AISTUDIO_TEST(ImpactAnalyzer_Analyze_FindsTransitiveIncluders) {
    TempProject project;
    // c.hpp includes b.hpp includes a.hpp: changing a.hpp affects both
    // b.hpp (direct) and c.hpp (transitive, two #include hops away).
    project.WriteFile("a.hpp", "void Foo();\n");
    project.WriteFile("b.hpp", "#include \"a.hpp\"\n");
    project.WriteFile("c.hpp", "#include \"b.hpp\"\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.Analyze("a.hpp");

    AISTUDIO_EXPECT(result.affected_files.size() == 2);
    AISTUDIO_EXPECT(Contains(result.affected_files, "b.hpp"));
    AISTUDIO_EXPECT(Contains(result.affected_files, "c.hpp"));
}

AISTUDIO_TEST(ImpactAnalyzer_Analyze_TargetFileItself_IsNotInAffectedFiles) {
    TempProject project;
    project.WriteFile("a.hpp", "void Foo();\n");
    project.WriteFile("b.hpp", "#include \"a.hpp\"\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.Analyze("a.hpp");

    AISTUDIO_EXPECT(!Contains(result.affected_files, "a.hpp"));
}

AISTUDIO_TEST(ImpactAnalyzer_Analyze_IncludeCycle_DoesNotInfiniteLoop) {
    TempProject project;
    // a.hpp <-> b.hpp form a cycle (not valid C++ without include guards
    // in practice, but IncludeGraph only sees text, not the preprocessor
    // guard) — the BFS must still terminate.
    project.WriteFile("a.hpp", "#include \"b.hpp\"\n");
    project.WriteFile("b.hpp", "#include \"a.hpp\"\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.Analyze("a.hpp");

    AISTUDIO_EXPECT(result.affected_files.size() == 1);
    AISTUDIO_EXPECT(Contains(result.affected_files, "b.hpp"));
}

AISTUDIO_TEST(ImpactAnalyzer_Analyze_ListsFunctionsDefinedInTargetFile) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\nvoid Bar() {\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.Analyze("a.cpp");

    AISTUDIO_EXPECT(result.affected_symbols.size() == 2);
    AISTUDIO_EXPECT(FindSymbol(result.affected_symbols, "Foo") != nullptr);
    AISTUDIO_EXPECT(FindSymbol(result.affected_symbols, "Bar") != nullptr);
}

AISTUDIO_TEST(ImpactAnalyzer_Analyze_FindsCallersOfSymbolsDefinedInTargetFile) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");
    project.WriteFile("b.cpp", "void Caller() {\n    Foo();\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.Analyze("a.cpp");

    const auto* foo = FindSymbol(result.affected_symbols, "Foo");
    AISTUDIO_EXPECT(foo != nullptr);
    AISTUDIO_EXPECT(foo->callers.size() == 1);
    AISTUDIO_EXPECT(foo->callers.front().caller_name == "Caller");
    AISTUDIO_EXPECT(foo->callers.front().caller_file == "b.cpp");
}

AISTUDIO_TEST(ImpactAnalyzer_Analyze_NoIncludersOrCallers_ReturnsEmptyLists) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.Analyze("a.cpp");

    AISTUDIO_EXPECT(result.affected_files.empty());
    const auto* foo = FindSymbol(result.affected_symbols, "Foo");
    AISTUDIO_EXPECT(foo != nullptr);
    AISTUDIO_EXPECT(foo->callers.empty());
}

AISTUDIO_TEST(ImpactAnalyzer_Analyze_UnknownFile_ReturnsEmptyResult) {
    TempProject project;
    project.WriteFile("a.cpp", "void Foo() {\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(project.RootString());
    CallGraph call_graph;
    call_graph.Build(project.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(project.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.Analyze("does_not_exist.cpp");

    AISTUDIO_EXPECT(result.affected_files.empty());
    AISTUDIO_EXPECT(result.affected_symbols.empty());
}

AISTUDIO_TEST(ImpactAnalyzer_AnalyzeChanges_DetectsOnlyTheTouchedFunction) {
    TempGitRepo repo;
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 1;\n}\n\nvoid Bar() {\n    int y = 2;\n}\n");
    repo.Commit("initial");
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 999;\n}\n\nvoid Bar() {\n    int y = 2;\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.AnalyzeChanges(repo.Diff());

    AISTUDIO_EXPECT(Contains(result.changed_files, "a.cpp"));
    AISTUDIO_EXPECT(FindChangedSymbol(result.changed_symbols, "Foo") != nullptr);
    AISTUDIO_EXPECT(FindChangedSymbol(result.changed_symbols, "Bar") == nullptr);
}

AISTUDIO_TEST(ImpactAnalyzer_AnalyzeChanges_ReportsCallersOfChangedFunction) {
    TempGitRepo repo;
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 1;\n}\n");
    repo.WriteFile("b.cpp", "void Caller() {\n    Foo();\n}\n");
    repo.Commit("initial");
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 999;\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.AnalyzeChanges(repo.Diff());

    const auto* foo = FindChangedSymbol(result.changed_symbols, "Foo");
    AISTUDIO_EXPECT(foo != nullptr);
    AISTUDIO_EXPECT(foo->kind == SymbolKind::Function);
    AISTUDIO_EXPECT(foo->callers.size() == 1);
    AISTUDIO_EXPECT(foo->callers.front().caller_name == "Caller");
    AISTUDIO_EXPECT(foo->callers.front().caller_file == "b.cpp");
}

AISTUDIO_TEST(ImpactAnalyzer_AnalyzeChanges_AffectedFiles_UnionsTransitiveIncludersOfEveryChangedFile) {
    TempGitRepo repo;
    // c.hpp includes b.hpp includes a.hpp -- changing a.hpp affects both.
    repo.WriteFile("a.hpp", "void Foo();\n");
    repo.WriteFile("b.hpp", "#include \"a.hpp\"\n");
    repo.WriteFile("c.hpp", "#include \"b.hpp\"\n");
    repo.Commit("initial");
    repo.WriteFile("a.hpp", "void Foo(int x);\n");

    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.AnalyzeChanges(repo.Diff());

    AISTUDIO_EXPECT(Contains(result.affected_files, "b.hpp"));
    AISTUDIO_EXPECT(Contains(result.affected_files, "c.hpp"));
}

AISTUDIO_TEST(ImpactAnalyzer_AnalyzeChanges_CleanWorkingTree_ReturnsEmptyResult) {
    TempGitRepo repo;
    repo.WriteFile("a.cpp", "void Foo() {\n}\n");
    repo.Commit("initial");

    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.AnalyzeChanges(repo.Diff());

    AISTUDIO_EXPECT(result.changed_files.empty());
    AISTUDIO_EXPECT(result.changed_symbols.empty());
    AISTUDIO_EXPECT(result.affected_files.empty());
}

AISTUDIO_TEST(ImpactAnalyzer_AnalyzeChanges_NewFunctionAppendedToTrackedFile_IsDetected) {
    TempGitRepo repo;
    // The blank separator line is committed BEFORE Baz is added, so it
    // shows up as unchanged context in the diff rather than as part of
    // the "+" span -- otherwise it would land exactly on the boundary
    // Symbol::line's "no end-line" heuristic uses for Foo's own
    // approximated span (up to Baz's line minus 1), a known imprecision
    // documented on ImpactAnalyzer::AnalyzeChanges().
    repo.WriteFile("a.cpp", "void Foo() {\n}\n\n");
    repo.Commit("initial");
    repo.WriteFile("a.cpp", "void Foo() {\n}\n\nvoid Baz() {\n    int z = 1;\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.AnalyzeChanges(repo.Diff());

    AISTUDIO_EXPECT(FindChangedSymbol(result.changed_symbols, "Baz") != nullptr);
    AISTUDIO_EXPECT(FindChangedSymbol(result.changed_symbols, "Foo") == nullptr);
}

AISTUDIO_TEST(ImpactAnalyzer_AnalyzeChanges_ViaDiffHead_DetectsStagedOnlyChange) {
    TempGitRepo repo;
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 1;\n}\n");
    repo.Commit("initial");
    repo.WriteFile("a.cpp", "void Foo() {\n    int x = 999;\n}\n");
    repo.RunGit({"add", "a.cpp"}); // staged, not committed -- invisible to plain "git diff"

    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);

    // The plain (unstaged-only) diff sees nothing -- confirms this test
    // actually exercises the staged case, not a false pass.
    AISTUDIO_EXPECT(analyzer.AnalyzeChanges(repo.Diff()).changed_symbols.empty());

    const auto result = analyzer.AnalyzeChanges(repo.DiffHead());
    AISTUDIO_EXPECT(FindChangedSymbol(result.changed_symbols, "Foo") != nullptr);
}

// Regression test for a diff-parser bug this session's own audit found by
// actually constructing the scenario with real git (docs/ROADMAP.md
// "ProcessRunnerのクォート処理の監査" sibling finding): an ADDED content
// line whose own text starts with "++ " renders, once git prepends its
// own '+' marker, as a line starting with "+++ " -- indistinguishable
// from a real file-header line by a naive string-prefix check. Before
// the fix, this silently misattributed the rest of the CURRENT file's
// hunk (here, Bar()) to a bogus phantom "file", so Bar's own change went
// undetected -- a false negative, not a crash, but still test-worthy.
AISTUDIO_TEST(ImpactAnalyzer_AnalyzeChanges_AddedLineLooksLikeDiffHeader_DoesNotConfuseParser) {
    TempGitRepo repo;
    repo.WriteFile("a.cpp", "void Foo() {\n}\n");
    repo.Commit("initial");
    repo.WriteFile("a.cpp", "void Foo() {\n}\n\n++ this looks like a header\nvoid Bar() {\n}\n");

    IncludeGraph include_graph;
    include_graph.Build(repo.RootString());
    CallGraph call_graph;
    call_graph.Build(repo.RootString());
    SymbolIndex symbol_index;
    symbol_index.Build(repo.RootString());

    const ImpactAnalyzer analyzer(include_graph, call_graph, symbol_index);
    const auto result = analyzer.AnalyzeChanges(repo.Diff());

    AISTUDIO_EXPECT(Contains(result.changed_files, "a.cpp"));
    AISTUDIO_EXPECT(FindChangedSymbol(result.changed_symbols, "Bar") != nullptr);
    // The confusing line must not have spawned a second, bogus
    // "changed_files" entry either.
    AISTUDIO_EXPECT(result.changed_files.size() == 1);
}
