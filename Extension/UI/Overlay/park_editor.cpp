#include "park_editor_internal.h"
#include <algorithm>
#include <format>

namespace dingosdk::overlay {
using namespace park_editor_detail;
namespace park_editor_detail {
namespace {
constexpr const char *middot = "\xc2\xb7";
// Text over the world: a soft shadow keeps it readable on a bright sky or a white ramp.
void shadowed_text(ImDrawList *draw, ImVec2 at, ImU32 colour, const char *text) {
    draw->AddText({at.x + 1, at.y + 1}, IM_COL32(0, 0, 0, 200), text);
    draw->AddText(at, colour, text);
}
} // namespace
void leave(ParkEditorUI &ui, EditorFrame &frame, bool play) {
    const auto &callbacks = frame.callbacks;
    if (ui.exit_pending || !callbacks.queue_debug)
        return;
    std::array<char, 512> result{};
    if (callbacks.queue_debug(callbacks.user, {DebugAction::set_park_editor, false}, result.data(), result.size())) {
        ui.exit_pending = true;
        ui.exit_at = ImGui::GetTime();
        frame.visible = !play;
        ui.flying = false;
        ui.dragging = -1;
        ui.placing.clear();
    }
    ui.feedback = result.data();
    ui.feedback_until = ImGui::GetTime() + 5;
}
void open_project_dialog(ParkEditorUI &ui, const Model &model) {
    const auto &state = model.editor;
    const bool has_project = std::any_of(state.park_mods.begin(), state.park_mods.end(),
                                         [&](const auto &mod) { return mod.folder == state.project; });
    ui.project_selected = has_project ? state.project : std::string();
    ui.project_open = true;
}
void save_project(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks) {
    if (!model.editor.project.empty())
        send(ui, model, callbacks, "mod-save", model.editor.project);
    else {
        ui.project_selected.clear();
        ui.project_open = true;
    }
}
int shown_layout(const ParkEditorUI &ui, const Model &model) {
    return ui.layout_pending.value_or(model.park_editor_layout) == park_editor_layout_rail ? park_editor_layout_rail
                                                                                            : park_editor_layout_hotbar;
}
void set_layout(ParkEditorUI &ui, const CallbacksV3 &callbacks, int layout) {
    layout = layout == park_editor_layout_rail ? park_editor_layout_rail : park_editor_layout_hotbar;
    // Shown at once; the saved setting comes back through the model a frame or more later.
    ui.layout_pending = layout;
    ui.layout_until = ImGui::GetTime() + 3.0;
    ui.snap_open = false;
    if (!callbacks.queue_console_command)
        return;
    std::array<char, 256> reply{};
    callbacks.queue_console_command(callbacks.user,
                                    layout == park_editor_layout_rail ? "editor layout rail" : "editor layout hotbar",
                                    reply.data(), reply.size());
}
void set_mode(ParkEditorUI &ui, ParkTransformMode mode) {
    if (mode == ParkTransformMode::scale && !ui.scaling_allowed)
        return;
    ui.mode = mode;
    ui.dragging = -1;
}
void stop_placing(ParkEditorUI &ui, const Model &model) {
    if (ui.preview_token && ui.preview_place)
        cancel_drag(ui, model);
    ui.placing.clear();
    ui.placing_set.clear();
    ui.surface_confirm = false;
}
void show_library(ParkEditorUI &ui, ParkLibraryView view) {
    ui.library_view = view;
    ui.library_hidden = false;
    ui.rail_panel = view == ParkLibraryView::sets    ? ParkRailPanel::sets
                    : view == ParkLibraryView::scene ? ParkRailPanel::scene
                                                     : ParkRailPanel::place;
}
void set_rail_panel(ParkEditorUI &ui, ParkRailPanel panel) {
    ui.rail_panel = panel;
    if (panel == ParkRailPanel::sets)
        ui.library_view = ParkLibraryView::sets;
    else if (panel == ParkRailPanel::scene)
        ui.library_view = ParkLibraryView::scene;
    else if (panel == ParkRailPanel::place &&
             (ui.library_view == ParkLibraryView::sets || ui.library_view == ParkLibraryView::scene))
        ui.library_view = ParkLibraryView::objects;
}
std::string project_name(const Model &model) {
    const auto &state = model.editor;
    for (const auto &mod : state.park_mods)
        if (mod.folder == state.project)
            return mod.title.empty() ? mod.folder : mod.title;
    return {};
}
std::string project_version(const Model &model) {
    const auto &state = model.editor;
    for (const auto &mod : state.park_mods)
        if (mod.folder == state.project)
            return mod.version;
    return {};
}
std::string status_message(const ParkEditorUI &ui, const Model &model, bool &fresh) {
    fresh = ui.pending || (!ui.feedback.empty() && ImGui::GetTime() < ui.feedback_until);
    if (ui.pending)
        return "Waiting for the game to apply the edit...";
    if (fresh)
        return ui.feedback;
    return model.editor.status;
}
std::string counts_text(const Model &model, bool autosave) {
    const auto count = model.editor.objects.size();
    auto text = std::to_string(count) + (count == 1 ? " object" : " objects");
    // A server holding this player to a number of objects: how many more they may place.
    const auto &mp = model.multiplayer;
    if (mp.object_limit_own)
        text += std::format("  {}  {} left on this server", middot,
                            mp.object_limit_own > mp.objects_placed ? mp.object_limit_own - mp.objects_placed : 0u);
    return autosave ? text + "  " + middot + "  layout autosaves" : text;
}
const char *viewport_hint(const ParkEditorUI &ui) {
    if (!ui.placing_set.empty())
        return "Click to place the set  \xc2\xb7  Wheel turns it  \xc2\xb7  Esc cancels";
    if (!ui.placing.empty())
        return "Click to place  \xc2\xb7  Esc cancels";
    return "T move / R rotate / Y scale  \xc2\xb7  Shift-click adds  \xc2\xb7  RMB flies";
}
void viewport_area(ParkEditorUI &ui, EditorFrame &frame, const char *id, ImVec2 at, ImVec2 size, ImVec2 note_at,
                   bool with_hint) {
    const auto &state = frame.model.editor;
    ImGui::SetCursorScreenPos(at);
    ImGui::BeginChild(id, {std::max(20.0f, size.x), std::max(20.0f, size.y)}, 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    viewport(ui, frame.model, frame.origin, frame.size, frame.blocked);
    auto *draw = ImGui::GetWindowDrawList();
    const float line = ImGui::GetTextLineHeightWithSpacing();
    if (ui.viewport_note)
        shadowed_text(draw, note_at, paper, ui.viewport_note);
    if (with_hint)
        shadowed_text(draw, {note_at.x, note_at.y + line}, IM_COL32(227, 232, 236, 255), viewport_hint(ui));
    const auto highlight = frame.blocked || ui.exit_pending ? std::vector<std::uint64_t>{} : ui.selection;
    if (highlight != ui.highlighted || ImGui::GetTime() >= ui.selection_heartbeat) {
        if (const auto queue = selection_queue.load(); queue && queue({state.generation, highlight})) {
            ui.highlighted = highlight;
            ui.selection_heartbeat = ImGui::GetTime() + .25;
        }
    }
    ImGui::EndChild();
}
float keycaps_width(const ParkEditorUI &ui, std::span<const Keycap> keys) {
    const float k = ui.ui_scale;
    ImFont *font = font_or(ui.bold_font);
    float width = 0;
    for (const auto &[key, label] : keys)
        width += font->CalcTextSizeA(12 * k, FLT_MAX, 0, key).x + font->CalcTextSizeA(14 * k, FLT_MAX, 0, label).x +
                 20 * k + 18 * k;
    return keys.empty() ? 0 : width - 18 * k;
}
float draw_keycaps(const ParkEditorUI &ui, ImDrawList *draw, ImVec2 at, std::span<const Keycap> keys) {
    const float k = ui.ui_scale;
    float x = at.x;
    for (const auto &[key, label] : keys)
        x += theme::keycap(draw, font_or(ui.bold_font), k, {x, at.y}, key, label) + 18 * k;
    return keys.empty() ? 0 : x - at.x - 18 * k;
}
void status_line(ParkEditorUI &ui, EditorFrame &frame, ImVec2 at, ImVec2 size, std::span<const Keycap> keys,
                 bool autosave) {
    const float k = frame.k;
    const auto P = [k](float value) { return value * k; };
    ImGui::SetCursorScreenPos(at);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::BeginChild("status-line", size, 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, plus(at, size), bar_colour);
    draw->AddRectFilled(at, {at.x + P(4), at.y + size.y}, blue);
    bool fresh{};
    const auto status = status_message(ui, frame.model, fresh);
    const auto counts = counts_text(frame.model, autosave);
    const float counts_width = ImGui::CalcTextSize(counts.c_str()).x;
    const float right = at.x + size.x - P(16);
    const float caps = keycaps_width(ui, keys);
    // A fresh message (an error, a save) outranks the prompts; the counts go last when space runs out.
    const bool show_counts = size.x > counts_width + P(420);
    const float message_right = show_counts ? right - counts_width - P(24) : right;
    const bool show_caps = !fresh && message_right - (at.x + P(18)) > caps + P(160);
    float x = at.x + P(18);
    if (show_caps)
        x += draw_keycaps(ui, draw, {x, at.y + (size.y - P(20)) * .5f}, keys) + P(24);
    const auto text = status.empty() && !show_caps ? std::string("Current layout autosaves. Save a named park to keep a reusable copy.")
                                                   : status;
    if (!text.empty() && message_right > x) {
        draw->PushClipRect({x, at.y}, {message_right, at.y + size.y}, true);
        draw->AddText({x, at.y + (size.y - ImGui::GetTextLineHeight()) * .5f}, fresh ? paper : muted, text.c_str());
        draw->PopClipRect();
        ImGui::SetCursorScreenPos({x, at.y});
        ImGui::InvisibleButton("##status", {std::max(1.0f, message_right - x), size.y});
        if (!status.empty() && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", status.c_str());
    }
    if (show_counts)
        draw->AddText({right - counts_width, at.y + (size.y - ImGui::GetTextLineHeight()) * .5f},
                      IM_COL32(185, 187, 192, 255), counts.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();
}
} // namespace park_editor_detail

void set_park_surface_queue(ParkSurfaceQueue queue) noexcept {
    surface_queue.store(queue);
}
void set_park_ground_queue(ParkGroundQueue queue) noexcept {
    ground_queue.store(queue);
}
void set_park_preview_queue(ParkPreviewQueue queue) noexcept {
    preview_queue.store(queue);
}
void set_park_selection_queue(ParkSelectionQueue queue) noexcept {
    selection_queue.store(queue);
}
void set_park_paste_queue(ParkPasteQueue queue) noexcept {
    paste_queue.store(queue);
}
void set_park_object_set_queue(ParkObjectSetQueue queue) noexcept {
    object_set_queue.store(queue);
}
void set_park_delete_queue(ParkDeleteQueue queue) noexcept {
    delete_queue.store(queue);
}
bool draw_park_editor(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks, bool console_open,
                      bool exit_requested, bool &visible) {
    const auto &state = model.editor;
    auto &io = ImGui::GetIO();
    // Esc cancels a drag or placement in progress, or closes the snap settings; with nothing in
    // progress it closes the editor and returns to the game. An open dialog closes itself.
    bool escape_closes = false;
    if (!console_open && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        const bool in_progress = ui.dragging >= 0 || !ui.placing.empty() || !ui.placing_set.empty() ||
                                 ui.preview_token || ui.surface_confirm || ui.inspector_dirty;
        if (in_progress) {
            cancel_drag(ui, model);
            ui.placing.clear();
            ui.placing_set.clear();
            ui.dragging = -1;
            ui.inspector_dirty = false;
            ui.draft_revision = 0;
            ui.surface_confirm = false;
        } else if (ui.snap_open)
            ui.snap_open = false;
        else
            escape_closes = true;
    }
    if (ui.generation != state.generation) {
        ui.generation = state.generation;
        ui.selected = 0;
        ui.selection.clear();
        ui.paste_expected.clear();
        ui.paste_before.clear();
        ui.preview_token = 0;
        ui.pick_confirm = ui.asset_drag = false;
        ui.dragging = -1;
        ui.placing.clear();
        ui.placing_set.clear();
        ui.set_delete.clear();
        ui.group_ids.clear();
        ui.paste_rotation.reset();
        ui.pending = false;
        ui.inspector_dirty = false;
        ui.initialized = false;
        ui.surface_confirm = false;
        ui.surface_request = {};
        ui.ground_request = {};
    }
    if (!ui.initialized) {
        ui.plane_height = model.debug.skater_position_valid ? model.debug.skater_position[1]
                                                            : model.debug.camera_position[1] - 2;
        ui.initialized = true;
    }
    if (ui.exit_pending && ImGui::GetTime() > ui.exit_at + 3) {
        ui.exit_pending = false;
        ui.feedback = model.debug.status;
        ui.feedback_until = ImGui::GetTime() + 5;
    }
    if (ui.layout_pending && (*ui.layout_pending == model.park_editor_layout || ImGui::GetTime() > ui.layout_until))
        ui.layout_pending.reset();
    if (ui.pending && !state.busy &&
        (state.revision != ui.queued_revision || state.status != ui.queued_status ||
         ImGui::GetTime() > ui.queued_at + 12))
        ui.pending = false;
    if (!ui.pending && !state.busy && !ui.paste_expected.empty())
        select_pasted(ui, state);
    if (ui.preview_token && (state.failed || (!state.busy && state.revision > ui.preview_revision))) {
        ui.preview_token = 0;
        ui.dragging = -1;
        ui.placing.clear();
        ui.free_release = ui.surface_confirm = false;
        refresh_selection(ui, state);
    }
    if ((ui.selected || !ui.selection.empty()) && !ui.inspector_dirty && ui.dragging < 0 && !ui.pending &&
        ui.draft_revision != state.revision)
        refresh_selection(ui, state);
    const float k = std::clamp(ui.ui_scale, .5f, 3.0f);
    const auto restore_font_scale = io.FontGlobalScale;
    io.FontGlobalScale = k;
    const auto *main = ImGui::GetMainViewport();
    const bool busy =
        state.busy || state.failed || ui.pending || ui.preview_token || ui.exit_pending || !state.available;
    EditorFrame frame{model, callbacks, visible, main->Pos, main->Size, k, busy, false};
    if (exit_requested || escape_closes) {
        if (state.busy || ui.pending || ui.preview_token) {
            ui.feedback = "Wait for the current edit before closing the editor.";
            ui.feedback_until = ImGui::GetTime() + 5;
        } else
            leave(ui, frame, escape_closes); // Esc goes straight back to the game
    }
    // A server that has turned object scaling off shares everything at its own size: nothing is
    // resized here either, so what this player sees is what everyone else does.
    ui.scaling_allowed = model.multiplayer.object_scaling;
    if (!ui.scaling_allowed) {
        ui.scale = 1;
        if (ui.mode == ParkTransformMode::scale) ui.mode = ParkTransformMode::move;
    }
    namespace skate = dingosdk::skate_theme;
    ImGui::SetNextWindowPos(frame.origin);
    ImGui::SetNextWindowSize(frame.size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {frame.px(9), frame.px(6)});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {frame.px(8), frame.px(6)});
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 0);
    const int colours = skate::push_widget_colours();
    const auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                       ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus |
                       ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("ReSkate Park Editor", nullptr, flags);
    project_dialog(ui, frame);
    frame.blocked = console_open || ui.exit_pending ||
                    ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                    io.AppFocusLost;
    const int layout = shown_layout(ui, model);
    if (layout == park_editor_layout_rail)
        rail_layout(ui, frame);
    else
        hotbar_layout(ui, frame);
    if (!console_open && !io.WantTextInput && !ImGui::IsAnyItemActive() && !frame.blocked) {
        if (!busy) {
            if (!io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
                if (ImGui::IsKeyPressed(ImGuiKey_T, false))
                    set_mode(ui, ParkTransformMode::move);
                if (ImGui::IsKeyPressed(ImGuiKey_R, false))
                    set_mode(ui, ParkTransformMode::rotate);
                if (ImGui::IsKeyPressed(ImGuiKey_Y, false))
                    set_mode(ui, ParkTransformMode::scale);
            }
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
                copy_selection(ui, state);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
                paste_objects(ui, state);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false) && (ui.selected || !ui.selection.empty()))
                duplicate_selection(ui, state);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))
                save_project(ui, model, callbacks);
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z))
                send(ui, model, callbacks, io.KeyShift ? "redo" : "undo");
            if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y))
                send(ui, model, callbacks, "redo");
            if (ui.selected && ui.selection.size() <= 1 && ImGui::IsKeyPressed(ImGuiKey_Delete))
                send(ui, model, callbacks, "delete");
            else if (ui.selection.size() > 1 && ImGui::IsKeyPressed(ImGuiKey_Delete, false))
                delete_selection(ui, state);
        }
        // The library and panel keys. Not while flying: the camera may use some of these letters.
        if (!ui.flying && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
            const bool rail = layout == park_editor_layout_rail;
            if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
                stop_placing(ui, model);
            if (ImGui::IsKeyPressed(ImGuiKey_P, false))
                rail ? set_rail_panel(ui, ParkRailPanel::place) : show_library(ui, ParkLibraryView::objects);
            if (ImGui::IsKeyPressed(ImGuiKey_G, false))
                rail ? set_rail_panel(ui, ParkRailPanel::sets) : show_library(ui, ParkLibraryView::sets);
            if (ImGui::IsKeyPressed(ImGuiKey_L, false))
                rail ? set_rail_panel(ui, ParkRailPanel::scene) : show_library(ui, ParkLibraryView::scene);
            if (ImGui::IsKeyPressed(ImGuiKey_N, false)) {
                if (rail)
                    set_rail_panel(ui, ui.rail_panel == ParkRailPanel::snap ? ParkRailPanel::place : ParkRailPanel::snap);
                else
                    ui.snap_open = !ui.snap_open;
            }
            if (!rail && ImGui::IsKeyPressed(ImGuiKey_H, false))
                ui.library_hidden = !ui.library_hidden;
            const bool chips = !rail || ui.rail_panel == ParkRailPanel::place;
            if (chips && ImGui::IsKeyPressed(ImGuiKey_LeftBracket))
                step_library_chip(ui, model, -1, !rail);
            if (chips && ImGui::IsKeyPressed(ImGuiKey_RightBracket))
                step_library_chip(ui, model, 1, !rail);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(colours);
    ImGui::PopStyleVar(10);
    io.FontGlobalScale = restore_font_scale;
    return ui.flying && !frame.blocked && !io.WantTextInput && ui.dragging < 0;
}
} // namespace dingosdk::overlay
