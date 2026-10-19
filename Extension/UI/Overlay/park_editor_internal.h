#pragma once
#include "park_editor.h"
#include "skate_style.h"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>

// Shared by the park editor's files (park_editor*.cpp).
namespace dingosdk::overlay::park_editor_detail {
inline std::atomic<ParkSurfaceQueue> surface_queue{};
inline std::atomic<ParkPreviewQueue> preview_queue{};
inline std::atomic<ParkSelectionQueue> selection_queue{};
inline std::atomic<ParkPasteQueue> paste_queue{};
inline std::atomic<ParkObjectSetQueue> object_set_queue{};
inline std::atomic<ParkDeleteQueue> delete_queue{};
using theme::blue;
using theme::paper;
inline constexpr ImU32 accent = skate_theme::blue, muted = skate_theme::grey_text;
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

// The object library and transform panels (park_editor_panels.cpp).
void library(ParkEditorUI &ui, const Model &model);
void inspector(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks);

// The 3D viewport: placement, gizmos and picking (park_editor_viewport.cpp).
void viewport(ParkEditorUI &ui, const Model &model, ImVec2 screen_origin, ImVec2 screen_size, bool blocked);
} // namespace dingosdk::overlay::park_editor_detail
