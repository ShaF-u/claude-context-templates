#pragma once

#include <string>
#include <vector>

namespace aistudio::core {

// A minimal, declarative vocabulary a Backend describes its own optional
// GUI panel with (docs/ROADMAP.md Phase 11 "UI extensions") -- the GUI
// (Gui/src/main.cpp, a separate process from aistudio_core_cli, but one
// that links aistudio_core directly and can load the same Plugin DLLs
// into its own process) renders whatever this returns, translating each
// element into the corresponding Dear ImGui call itself.
//
// Deliberately small (three element kinds) rather than a general-purpose
// UI framework: ImGui's own API is immediate-mode and far too large a
// surface to cross a stable ABI boundary function-by-function (unlike
// the Backend Command/Query/Context Provider ABI, which is a handful of
// JSON-in/JSON-out calls). A Plugin that needs genuinely custom
// rendering is out of scope for this vocabulary -- widening it (or a
// different approach, like exposing a curated C ABI over ImGui itself)
// is a future decision, not something this struct tries to anticipate.
enum class UiElementKind {
    Text,
    Separator,
    // A clickable button. Clicking it dispatches Command{.name =
    // UiElement::action_id} to the same IBackend that produced this
    // UiPanelDescription -- reusing IBackend::Dispatch() rather than
    // inventing a second action-handling path, so a Backend that already
    // implements Dispatch() needs nothing new to handle a button click.
    Button,
};

struct UiElement {
    UiElementKind kind = UiElementKind::Text;
    std::string text;       // Text: the line's content. Button: its label. Unused for Separator.
    std::string action_id;  // Button only: the Command name Dispatch() receives on click.
};

// A Backend's optional GUI panel, one per Backend that has anything to
// show -- see IBackend::RenderUiDescription(). An empty `elements` (the
// default-constructed value) means "nothing to render"; the GUI shows no
// panel for a Backend that returns this.
struct UiPanelDescription {
    std::string title;
    std::vector<UiElement> elements;
};

} // namespace aistudio::core
