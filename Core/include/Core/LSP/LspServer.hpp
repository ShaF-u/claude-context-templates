#pragma once

#include "Core/Index/IncludeGraph.hpp"
#include "Core/Index/SymbolIndex.hpp"
#include "Core/Security/Sandbox.hpp"

#include <istream>
#include <ostream>
#include <string>

namespace aistudio::core {

// A minimal, deliberately narrow first slice of the Language Server
// Protocol (docs/ROADMAP.md "## LSP"; CLAUDE.md's 2026-09-04 note that
// LSP needs its own new subsystem "one size bigger" than McpServer,
// since the wire protocol itself -- textDocument sync, position
// encoding -- has to be implemented, not just new tools bolted onto
// something that already speaks it).
//
// Scope for this first slice (AGENT.md #14 "minimal implementation
// first"): the lifecycle (initialize / initialized / shutdown / exit) +
// document sync (textDocument/didOpen / didChange / didClose, whole-
// document replacement only -- no incremental
// TextDocumentContentChangeEvent ranges) + five read-only capabilities,
// textDocument/definition, textDocument/documentSymbol,
// workspace/symbol, textDocument/references and
// textDocument/typeDefinition, all built directly on the existing
// SymbolIndex (no new symbol-resolution logic; see
// HandleDefinition/HandleDocumentSymbol/HandleWorkspaceSymbol/
// HandleReferences/HandleTypeDefinition's own comments in LspServer.cpp).
// Every other LSP request this doesn't recognize gets a MethodNotFound
// error, which is how the spec expects an LSP server to signal a
// capability it simply doesn't have -- no separate "unsupported
// capability" registration is needed since `initialize`'s own result
// never advertises anything beyond textDocumentSync/definitionProvider/
// documentSymbolProvider/workspaceSymbolProvider/referencesProvider/
// typeDefinitionProvider in the first place.
// References is a name-text heuristic, NOT built on ReferenceGraph/
// CallGraph -- see HandleReferences's own comment in LspServer.cpp for
// why (those two indexes were investigated and found to give zero
// coverage for two of SymbolIndex's five SymbolKinds). typeDefinition, by
// contrast, is NOT a heuristic -- a prior task's report claimed Type
// needed real type inference beyond tree-sitter's syntax-only parsing
// without verifying that claim; re-investigating for this task found
// tree-sitter-cpp's own declaration nodes already expose a "type" field
// structurally, the same way SymbolExtractor already reads their
// "declarator" field for a name, so Symbol::type_name (Symbol.hpp) now
// captures it directly at index-build time -- see HandleTypeDefinition's
// own comment in LspServer.cpp for exactly what it does and doesn't
// cover (builtins/qualified names/templates/`auto` are left unresolved
// rather than guessed at). textDocument/publishDiagnostics (index-derived)
// and textDocument/rename / textDocument/prepareRename (single-file
// scope, opt-in via LspServerOptions::enable_rename) are also
// implemented -- see their own doc comments further down this file and in
// LspServer.cpp. IDE-extension wiring (VS Code / Visual Studio / Rider)
// remains explicitly out of scope for this file -- see docs/ROADMAP.md's
// Current Status entries for the reasoning.
//
// documentSymbol/workspace symbol both return the flat, LSP 3.17
// SymbolInformation[] shape (not the hierarchical DocumentSymbol[]
// shape): SymbolIndex (Core/Index/SymbolIndex.hpp) is itself a flat
// std::vector<Symbol> with no parent/child containment relationship at
// all -- building a fake DocumentSymbol tree would mean inventing
// nesting data this project doesn't actually have, which AGENT.md #14/#15
// both rule out. SymbolInformation[] is the honest shape for what's
// really indexed.
//
// Framing is LSP's own `Content-Length: N\r\n\r\n<json>` header framing
// (verified against the LSP 3.17 specification's "Base Protocol"
// section) -- NOT McpServer's newline-delimited framing next door. The
// two protocols share JSON-RPC 2.0 as a message *format* but not a
// transport *framing*, so this class re-implements its own ReadMessage/
// WriteMessage rather than trying to share McpServer's stdio read loop.
//
// textDocument/publishDiagnostics (docs/ROADMAP.md's Diagnostics Design
// Proposal, now resolved by the user's explicit decision -- v1 scoped to
// INDEX-DERIVED diagnostics only, real compiler/build execution
// explicitly out of scope and deferred): a server-initiated NOTIFICATION
// (no `id`, no response expected), pushed after every
// textDocument/didOpen and textDocument/didChange for exactly the one
// affected document (the natural point where this server already has
// fresh document text -- see HandleDidOpen/HandleDidChange in
// LspServer.cpp), and cleared (an empty `diagnostics` array published)
// on textDocument/didClose if that document currently has any. This is
// the first message shape this server ever sends that ISN'T a direct
// response to a request/notification the client just sent -- framed via
// the exact same WriteMessage() as every response, just without an
// `id` field (see MakeNotification in LspServer.cpp). Two sources, both
// derived from indexes this project already builds rather than running
// any compiler:
//   * A parse failure -- tree-sitter-cpp itself couldn't cleanly parse
//     the document (an ERROR node or a synthesized MISSING token
//     anywhere in the tree) -- Severity::Error. See DetectParseError in
//     LspServer.cpp for why this re-parses the open buffer locally
//     rather than reusing AstExtractor/SymbolExtractor's own trees.
//   * An unresolved `#include "..."` (IncludeGraph::AllEdges() has an
//     edge for this file with an empty resolved_path) -- Severity::Warning,
//     not Error, since IncludeGraph's suffix-matching resolution is a
//     heuristic that can have false positives for a header reachable
//     only via a compiler include-search-path this project doesn't model
//     (see ComputeDiagnostics's own comment). `<...>` system includes are
//     deliberately excluded -- IncludeGraph::Build() never even attempts
//     to resolve them (its own "if (!edge.is_system)" gate), so an
//     unresolved system include is IncludeGraph's own by-design behavior,
//     not a defect to surface.
// `include_graph` below is nullptr-able like `symbol_index`/`firewall`:
// nullptr just skips the unresolved-include half, the parse-failure half
// works unconditionally whenever `project_root` is set. No new
// capability field is advertised in `initialize`'s result for this --
// unlike definitionProvider/etc., LSP's PUSH diagnostics model (as
// opposed to the newer 3.17 PULL `textDocument/diagnostic` request,
// which this server does not implement) requires no capability
// advertisement; a client either listens for the notification or
// ignores it.
//
// Real compiler/build diagnostics remain explicitly OUT OF SCOPE (the
// Design Proposal's "large, new Build subsystem" concern -- no
// ProcessRunner invocation, no async execution framework, nothing here
// runs a compiler) -- this is a deliberately smaller v1 slice building
// only on indexes this project already has.
//
// FORMERLY A KNOWN LIMITATION, FIXED by `feature/LspModeFileWatcher`
// (docs/ROADMAP.md's "未解決includeのstale診断" follow-up task): the
// unresolved-include half is driven entirely by `include_graph`, which
// USED TO be built once at LSP-server startup and never refreshed on
// didChange/didSave -- so editing an open document to remove the very
// `#include` line that triggered a warning would NOT clear that
// diagnostic. `RunLspMode()` (Core/src/main.cpp) now wires a FileWatcher
// that rebuilds `include_graph` (and `symbol_index`) via UpdateFile()/
// RemoveFile() on every "FileChanged" EventBus event, the same pattern
// `RunMcpMode()`'s own FileWatcher already used for its six indexes --
// so once the edited file is actually saved to disk and the watcher has
// had a chance to react, a subsequent didOpen/didChange for that
// document computes diagnostics against the refreshed graph. This is
// still the usual "point-in-time index" staleness this file's other
// capabilities already accept (see LocationFor's own comment) -- a
// change that hasn't yet landed on disk, or hasn't yet been picked up by
// the FileWatcher's background thread, is not instantly reflected -- but
// it's no longer PERMANENTLY stale for the lifetime of the process the
// way it was before this fix. See ComputeDiagnostics in LspServer.cpp
// for the diagnostic computation itself (unchanged by this fix -- it was
// always driven by whatever `include_graph` currently contains; only
// `include_graph`'s own freshness changed).
//
// Position encoding: LSP's Position.character counts UTF-16 code units
// within a line (the protocol's default, and only, `positionEncoding`
// this server advertises support for) -- NOT a byte offset or a
// Unicode-codepoint count. Core/Util/Utf16.hpp's
// Utf16OffsetToUtf8Byte()/Utf8ByteToUtf16Offset() do that conversion
// against this project's existing line-based (1-based `Symbol::line`,
// no column at all) position representation -- every other index in
// this codebase (SymbolIndex, AstIndex, ReferenceGraph, CallGraph, ...)
// only ever tracks a line number, never a column, so this conversion is
// purely an LSP-layer concern, not something the underlying indexes
// needed to grow.
struct LspServerOptions {
    // nullptr disables textDocument/definition, textDocument/documentSymbol,
    // workspace/symbol, textDocument/references, textDocument/typeDefinition
    // and (together with enable_rename below) textDocument/rename /
    // textDocument/prepareRename alike: `initialize`'s result won't
    // advertise definitionProvider/documentSymbolProvider/
    // workspaceSymbolProvider/referencesProvider/typeDefinitionProvider/
    // renameProvider capabilities, and a client that calls one of the
    // read-only ones anyway still gets a well-formed `null` result rather
    // than an error (Rename itself always errors when unavailable -- see
    // enable_rename's own comment on why that one capability is stricter).
    const SymbolIndex* symbol_index = nullptr;
    // Absolute filesystem path (no trailing slash), used to convert
    // between the `file://` URIs LSP speaks and the project-relative
    // paths SymbolIndex itself uses (the same convention FileScanner's
    // generic_string() paths already follow), in both directions --
    // definition/workspace-symbol only ever need path->URI (a resolved
    // Symbol::file_path becomes a Location), but documentSymbol also
    // needs the reverse, URI->path (see UriToProjectRelativePath in
    // LspServer.cpp), to know which SymbolIndex entries belong to the
    // requested document. Empty disables all three capabilities even if
    // symbol_index is set, since neither direction of that conversion is
    // possible without it.
    std::string project_root;
    // Context Firewall (docs/MASTER_SPEC.md #21), applied the same way
    // McpServerOptions::firewall is applied to every McpServer tool
    // result -- a definition/documentSymbol/workspace-symbol result
    // naming a file outside the allowed set is silently dropped rather
    // than exposed, matching this project's existing "every
    // externally-reachable path must go through Sandbox" invariant (see
    // docs/ROADMAP.md "Sandboxの広範な監査"). nullptr applies no additional
    // filtering.
    const Sandbox* firewall = nullptr;
    // Source for the unresolved-`#include` half of
    // textDocument/publishDiagnostics (see this header's own comment
    // above) -- nullptr skips just that half; the parse-failure half is
    // unaffected. Not required for any of the other capabilities above.
    const IncludeGraph* include_graph = nullptr;
    // Gates textDocument/rename and textDocument/prepareRename --
    // docs/ROADMAP.md's Rename entry has the full design writeup,
    // resolved by the user's explicit decisions: (1) single-FILE scope
    // only (the WorkspaceEdit this produces ever names exactly the one
    // document the request itself specified -- never expanded to the
    // whole project, unlike textDocument/references' project-wide scan),
    // and (2) this exact opt-in-toggle shape, deliberately mirroring
    // McpServerOptions::enable_git_write_commands (Core/MCP/McpServer.hpp)
    // -- defaults to false, an operator has to set
    // `mcp.enable_lsp_rename=true` in aistudio.config (see RunLspMode() in
    // Core/src/main.cpp) before renameProvider is even advertised. Kept
    // under the same "mcp." key family as enable_git_write_commands
    // (rather than an "lsp."-prefixed key) quite deliberately, even though
    // this flag lives on the LSP server, not McpServer: both flags gate
    // the exact same *kind* of thing -- an opt-in escape hatch for a
    // capability that goes beyond pure read-only lookup, delegating actual
    // approval to whichever MCP/LSP client's own permission mode is in
    // front of it rather than Studio building a new approval mechanism of
    // its own (see this repo's GitBackend write-command precedent,
    // docs/ROADMAP.md) -- so the shared naming makes that shared intent
    // discoverable by grepping "mcp.enable_" rather than being split
    // across two unrelated-looking prefixes.
    //
    // OWN RISK-FRAMING CONCLUSION (re-examining the Design Proposal's
    // GitBackend-write-command analogy against LspServer's ACTUAL
    // mechanism, now that it exists to read): textDocument/rename here
    // computes and returns a WorkspaceEdit -- the set of TextEdits the
    // CLIENT would need to apply -- it never writes to disk itself (see
    // HandleRename's own comment in LspServer.cpp; Run()'s document store
    // is only ever mutated by didOpen/didChange/didClose, never by
    // rename). That makes this capability READ-ONLY from this server's
    // own perspective, structurally unlike every GitBackend write Command
    // (git.commit/git.branch/git.stash/git.checkout/git.tag), each of
    // which actually mutates the working tree or its history the instant
    // Dispatch() runs, no client-side "apply" step in between, no
    // remaining chance to just not apply a bad result. A wrong Rename
    // computation here is exactly as recoverable as a wrong
    // textDocument/definition or textDocument/references result: the
    // client simply doesn't apply the edit (or applies it and the user's
    // own editor Undo reverts it) -- there is no analog to GitBackend's
    // "-f"/"--output=" injection vulnerabilities (docs/ROADMAP.md's
    // GitBackend implementation review) because there is no subprocess
    // argument-construction step here at all; every input (newName, the
    // cursor's own file/position) only ever flows into in-memory string
    // comparison/substring-copy logic, never into a shell command line.
    // The toggle is kept anyway -- not because this task's own re-reading
    // proves the write-command danger class applies, but because (a) the
    // false-positive precision risk below is real and distinct from the
    // GitBackend injection class, and (b) matching the established
    // opt-in-toggle convention costs nothing and keeps this project's
    // "capabilities beyond pure read lookup default OFF" invariant simple
    // and uniform for anyone auditing aistudio.config later, rather than
    // trying to argue Rename is "safe enough" to be the first exception.
    //
    // PRECISION SAFEGUARD AND ITS LIMITS (see ResolveRenameTarget's own
    // comment in LspServer.cpp for the full writeup): the cursor's
    // identifier must resolve to at least one real declared symbol
    // somewhere in the project (the same SymbolIndex::FindByName() exact-
    // name lookup every other capability in this file already uses) --
    // this rejects renaming plain text that was never a real declared
    // symbol at all (a comment word, a string-literal fragment, a typo).
    // It does NOT prove the specific occurrence under the cursor IS that
    // declaration: once the sanity check passes, every WHOLE-WORD textual
    // occurrence of the same spelling within the ONE target file is
    // rewritten, including an unrelated same-named identifier in a
    // different scope/class of that same file (e.g. a local variable `x`
    // in one function vs. a struct member `x` in another, both declared
    // in the file being renamed) -- the exact false-positive risk the
    // Design Proposal raised, now deliberately bounded to one file rather
    // than eliminated. This is the same "heuristic exact-name match, not
    // real semantic resolution" tradeoff textDocument/references already
    // documents and accepts.
    bool enable_rename = false;
    std::string server_name = "aistudio-core-lsp";
    std::string server_version = "0.1.0";
};

class LspServer {
public:
    explicit LspServer(LspServerOptions options) : options_(std::move(options)) {}

    // Reads Content-Length-framed JSON-RPC 2.0 messages from `in` and
    // writes framed responses to `out` until `in` reaches EOF, a
    // malformed frame is read, or an "exit" notification is received --
    // LSP's own lifecycle end, distinct from McpServer::Run's EOF-only
    // exit condition (a well-behaved LSP client sends shutdown then
    // exit; this method honors "exit" as the terminating signal
    // regardless of whether shutdown was requested first, matching real
    // clients that may skip the courtesy on an abrupt disconnect).
    // Single-threaded, blocking -- matches the stdio transport's own
    // framing (one message read to completion, then the next).
    void Run(std::istream& in, std::ostream& out) const;

private:
    LspServerOptions options_;
};

} // namespace aistudio::core
