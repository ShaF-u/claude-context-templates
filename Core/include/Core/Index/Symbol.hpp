#pragma once

#include <string>

namespace aistudio::core {

// What Project Intelligence's most basic unit represents — a named,
// locatable declaration in a source file (docs/ROADMAP.md Phase 3 "Code
// Intelligence"). Variable covers member variables (qualified with
// their enclosing class, e.g. "Foo::x") and namespace/global-scope
// variables — deliberately not local variables inside a function body,
// which would be far too numerous and rarely meaningful to search for
// project-wide (see SymbolExtractor's own note on this). Templates and
// overload sets still need real semantic analysis and are deferred.
enum class SymbolKind { Class, Struct, Function, Namespace, Variable };

[[nodiscard]] std::string ToString(SymbolKind kind);

struct Symbol {
    std::string name;
    SymbolKind kind = SymbolKind::Function;
    std::string file_path; // relative, matches FileMetadata::path
    int line = 0;          // 1-based
    // The declaration's header text — everything from the start of the
    // node up to (not including) its body, e.g. "int Foo::Bar(int x)
    // const" or "class Foo : public Bar". Empty for hand-constructed
    // Symbols (tests) or if extraction couldn't locate a body. Never
    // includes the body itself, so this stays a compact signature even
    // for large class/namespace definitions.
    std::string signature;
    // The name of this Function/Variable's declared type — e.g. "Foo" for
    // both "Foo x;" and "Foo Bar();" (a variable's declared type / a
    // function's return type). This is NOT text re-derived from
    // `signature` (that would mean re-parsing prose this project already
    // decided elsewhere not to trust — see AGENT.md #14/#15): it is
    // tree-sitter-cpp's own "type" field on the declaration/
    // field_declaration/function_definition node, read directly at
    // extraction time the same structural way SymbolExtractor already
    // reads that node's "declarator" field to get its name (see
    // SymbolExtractor.cpp's DeclaredTypeName). Left empty whenever that
    // field isn't itself a plain `type_identifier` — a builtin
    // (`primitive_type`, e.g. "int"/"bool"/"void"), a namespace-qualified
    // type (`qualified_identifier`, e.g. "std::string"), a template
    // instantiation (`template_type`, e.g. "std::vector<Foo>"), or `auto`
    // (`placeholder_type_specifier`, genuine type deduction) — since none
    // of those name something SymbolIndex::FindByName could ever resolve
    // anyway, and guessing at one (e.g. peeling off "std::" or template
    // arguments) is exactly the kind of unfounded inference this project
    // rules out. Always empty for Class/Struct/Namespace symbols (there
    // is no "type" of a type).
    std::string type_name;
};

} // namespace aistudio::core
