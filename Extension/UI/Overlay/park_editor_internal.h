#pragma once
#include "park_editor.h"
#include "skate_style.h"

#include <atomic>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared by the park editor's files (park_editor*.cpp).
namespace dingosdk::overlay::park_editor_detail {
inline std::atomic<ParkSurfaceQueue> surface_queue{};
inline std::atomic<ParkGroundQueue> ground_queue{};
inline std::atomic<ParkPreviewQueue> preview_queue{};
inline std::atomic<ParkSelectionQueue> selection_queue{};
inline std::atomic<ParkPasteQueue> paste_queue{};
inline std::atomic<ParkObjectSetQueue> object_set_queue{};
inline std::atomic<ParkDeleteQueue> delete_queue{};
using theme::blue;
using theme::paper;
inline constexpr ImU32 accent = skate_theme::blue, muted = skate_theme::grey_text;
// The editor's chrome: dark bars over the world and the panels' rough tiles.
inline constexpr ImU32 bar_colour = IM_COL32(10, 10, 11, 245), panel_colour = IM_COL32(20, 20, 21, 242);
inline ImFont *font_or(ImFont *font) {
    return font ? font : ImGui::GetFont();
}
inline ImVec2 plus(ImVec2 a, ImVec2 b) {
    return {a.x + b.x, a.y + b.y};
}

// Selection, live previews and editor commands (park_editor_actions.cpp).
const EditorObject *selected_object(const ParkEditorUI &ui, const ParkEditorModel &model);
void refresh_selection(ParkEditorUI &ui, const ParkEditorModel &model);
void choose(ParkEditorUI &ui, const ParkEditorModel &model, const EditorObject &object, bool extend);
void capture_drag(ParkEditorUI &ui, const ParkEditorModel &model);
void copy_selection(ParkEditorUI &ui, const ParkEditorModel &model);
void paste_objects(ParkEditorUI &ui, const ParkEditorModel &model);
void select_pasted(ParkEditorUI &ui, const ParkEditorModel &model);
// Deletes every selected object as one edit; with one selected, `send` "delete" does the same.
bool delete_selection(ParkEditorUI &ui, const ParkEditorModel &model);
// Copies every selected object at its saved position, rotation and scale, one grid step along X,
// as one paste; the copies are then selected (a group keeps the group's own axes).
bool duplicate_selection(ParkEditorUI &ui, const ParkEditorModel &model);
// An object from the library follows the cursor until a click places it; the object is remembered
// for the library's RECENT list.
void begin_placement(ParkEditorUI &ui, const Model &model, const std::string &key);
void remember_recent(ParkEditorUI &ui, const std::string &key);
// Saved object sets (park_editor_actions.cpp).
const EditorObjectSet *placing_object_set(const ParkEditorUI &ui, const ParkEditorModel &model);
std::vector<EditorObject> placed_set_objects(const ParkEditorUI &ui, const EditorObjectSet &set);
void begin_set_placement(ParkEditorUI &ui, const Model &model, const std::string &name);
bool place_set(ParkEditorUI &ui, const ParkEditorModel &model);
bool save_set(ParkEditorUI &ui, const ParkEditorModel &model);
bool delete_set(ParkEditorUI &ui, const ParkEditorModel &model, const std::string &name);
bool preview(ParkEditorUI &ui, const Model &model, EditorPreviewAction action);
void cancel_drag(ParkEditorUI &ui, const Model &model);
bool send(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks, std::string_view operation,
          std::string_view argument = {}, const std::vector<std::string> &texts = {});

// One frame of the editor as the layouts draw it (park_editor.cpp).
struct EditorFrame {
    const Model &model;
    const CallbacksV3 &callbacks;
    bool &visible;
    ImVec2 origin, size; // the screen
    float k;             // the menu's scale
    bool busy;           // an edit is in flight or the editor is unavailable: file, history and modes wait
    bool blocked;        // a dialog, the console or a lost focus has the input: the viewport waits
    float px(float value) const {
        return value * k;
    }
};
// The layouts (park_editor_hotbar.cpp, park_editor_rail.cpp). Both draw the shared pieces below.
void hotbar_layout(ParkEditorUI &ui, EditorFrame &frame);
void rail_layout(ParkEditorUI &ui, EditorFrame &frame);

// Actions the layouts' buttons and the keyboard share (park_editor.cpp).
void leave(ParkEditorUI &ui, EditorFrame &frame, bool play);
// SAVE and Ctrl+S: into the open park mod, or the PARK PROJECT dialog to make one.
void save_project(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks);
void open_project_dialog(ParkEditorUI &ui, const Model &model);
// The layout this frame is drawn in, and switching it: saved with the profile (`editor layout`).
int shown_layout(const ParkEditorUI &ui, const Model &model);
void set_layout(ParkEditorUI &ui, const CallbacksV3 &callbacks, int layout);
void set_mode(ParkEditorUI &ui, ParkTransformMode mode);
// Drops an object or set following the cursor (the rail's SELECT tool, Q).
void stop_placing(ParkEditorUI &ui, const Model &model);
void show_library(ParkEditorUI &ui, ParkLibraryView view);
void set_rail_panel(ParkEditorUI &ui, ParkRailPanel panel);
// The open park mod's name ("" while unsaved) and version.
std::string project_name(const Model &model);
std::string project_version(const Model &model);
// What the status line says: the latest message (fresh: just said, so it outranks the key prompts).
std::string status_message(const ParkEditorUI &ui, const Model &model, bool &fresh);
// "23 objects", "77 left on this server" while a server holds this player to a number, and with
// `autosave` "layout autosaves", between middle dots.
std::string counts_text(const Model &model, bool autosave);
// What the mouse does in the viewport right now.
const char *viewport_hint(const ParkEditorUI &ui);
// The 3D viewport in a child window at `at` (screen), with the selection highlight kept in step.
// `note_at` is where the viewport's own line goes (no surface hit, say), with the hint on the line
// under it when `with_hint`.
void viewport_area(ParkEditorUI &ui, EditorFrame &frame, const char *id, ImVec2 at, ImVec2 size, ImVec2 note_at,
                   bool with_hint);
// A row of keycaps with what each does, from `at`; returns the width drawn.
using Keycap = std::pair<const char *, const char *>;
float keycaps_width(const ParkEditorUI &ui, std::span<const Keycap> keys);
float draw_keycaps(const ParkEditorUI &ui, ImDrawList *draw, ImVec2 at, std::span<const Keycap> keys);
// The status line in the rectangle `at` + `size`: keycaps on the left (left out while a fresh message
// needs the room), the message, and the counts on the right (with the autosave note: `autosave`).
void status_line(ParkEditorUI &ui, EditorFrame &frame, ImVec2 at, ImVec2 size, std::span<const Keycap> keys,
                 bool autosave);

// Tiles and chips in the menu kit's colours (park_editor_panels.cpp).
enum class Chip {
    plain, // no tile until hovered, grey text: an action that is not the main one
    tile,  // a grey tile
    blue,  // chosen, or the main action: blue with black text
    white, // a switch that is on: skate.'s light grey with black text
    danger // a grey tile with red text
};
float chip_width(const ParkEditorUI &ui, const char *label, float text_size = 15);
// A chip at the cursor, `height` high and `width` wide (0: fits its label). Returns whether pressed.
bool chip(const ParkEditorUI &ui, const char *label, Chip style, float height, unsigned seed, bool enabled = true,
          float width = 0, float text_size = 15);
// A rough-cut tile with a child window on it at `at` (screen), for the floating bars and panels.
// Drawn in the child's own list so it covers the viewport's gizmo lines beneath it.
template <class Body>
void float_panel(const char *id, ImVec2 at, ImVec2 size, unsigned seed, float k, ImVec2 padding, Body &&body,
                 ImU32 colour = panel_colour, ImGuiChildFlags child_flags = 0, ImGuiWindowFlags window_flags = 0) {
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    if (ImGui::BeginChild(id, size, ImGuiChildFlags_AlwaysUseWindowPadding | child_flags,
                          window_flags | ImGuiWindowFlags_NoScrollbar)) {
        auto *draw = ImGui::GetWindowDrawList();
        const auto from = ImGui::GetWindowPos(), extent = ImGui::GetWindowSize();
        draw->PushClipRect(plus(from, {-2, -2}), plus(from, {extent.x + 2, extent.y + 2}), false);
        theme::tile_rect(draw, from, plus(from, extent), colour, seed, k);
        draw->PopClipRect();
        body();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}
// A section header like the menu's: heading text over a rule with a blue stub.
void heading(const ParkEditorUI &ui, const char *text);

// The object library (park_editor_panels.cpp). Its chips choose a category or a view (RECENT, and
// with `views` SETS and SCENE); [ and ] step them.
struct LibraryChip {
    std::string label, category;
    ParkLibraryView view{};
};
std::vector<LibraryChip> library_chips(const Model &model, bool views);
void step_library_chip(ParkEditorUI &ui, const Model &model, int step, bool views);
// The chips, wrapped over as many rows as they need (`wrap`), or in one row `width` wide (0: the
// rest of the line) that the wheel scrolls.
void library_chip_row(ParkEditorUI &ui, const Model &model, bool views, bool wrap, float height, float width = 0);
void library_search(ParkEditorUI &ui, const Model &model, float width);
// The objects of the library's view as a grid of cards (`columns` 0: as many as fit) or one strip of
// cards the wheel scrolls; click one or drag it into the world to place it.
void object_grid(ParkEditorUI &ui, const Model &model, int columns);
void object_strip(ParkEditorUI &ui, const Model &model, float card_width);
// Saved sets: name and save the selection, then the sets with Place / Delete, as a list or a strip.
void set_list(ParkEditorUI &ui, const Model &model);
void set_strip(ParkEditorUI &ui, const Model &model, float card_width);
// The placed objects; a click selects one, Shift-click adds it. `search` filters by ui.search.
void scene_list(ParkEditorUI &ui, const Model &model, bool search);
void scene_strip(ParkEditorUI &ui, const Model &model, float card_width);
// Position, rotation and scale of the selection or the object being placed, APPLY (or PLACE AT
// COORDINATES), SAVE AS SET, DUPLICATE and DELETE.
void transform_fields(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks);
// What the selection is, as a title and a line under it.
std::string selection_title(const ParkEditorUI &ui, const Model &model);
std::string selection_detail(const ParkEditorUI &ui, const Model &model);
// Surfaces or plane, snapping, the grid, angle and scale steps, the lift or build plane, local axes.
void placement_settings(ParkEditorUI &ui);
// The SNAP chip's label: the grid in metres and the angle step in degrees, or SNAP OFF.
std::string snap_label(const ParkEditorUI &ui);

// The PARK PROJECT dialog (park_editor_project.cpp).
void project_dialog(ParkEditorUI &ui, EditorFrame &frame);

// The 3D viewport: placement, gizmos and picking (park_editor_viewport.cpp).
void viewport(ParkEditorUI &ui, const Model &model, ImVec2 screen_origin, ImVec2 screen_size, bool blocked);
} // namespace dingosdk::overlay::park_editor_detail
