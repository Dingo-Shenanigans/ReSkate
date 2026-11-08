#include "park_editor_internal.h"
#include <algorithm>
#include <array>
#include <format>

// The tool rail layout: a top bar, a narrow rail of tools down the left, the panel the tool opens
// beside it, the viewport in the middle and the selection's inspector on the right.
namespace dingosdk::overlay::park_editor_detail {
namespace {
constexpr const char *middot = "\xc2\xb7";
enum class RailTool { place, select, move, rotate, scale, sets, scene, snap };
struct RailEntry {
    RailTool tool;
    const char *name, *key, *hint;
};
constexpr std::array<RailEntry, 8> rail_tools{{
    {RailTool::place, "PLACE", "P", "The object library: click or drag an object into the world."},
    {RailTool::select, "SELECT", "Q", "Stop placing and pick objects; Shift-click adds to the selection."},
    {RailTool::move, "MOVE", "T", "Move along the handles."},
    {RailTool::rotate, "ROTATE", "R", "Turn on the rings."},
    {RailTool::scale, "SCALE", "Y", "Scale with the handles."},
    {RailTool::sets, "SETS", "G", "Saved sets: save the selection, place a set again."},
    {RailTool::scene, "SCENE", "L", "Everything placed in the park."},
    {RailTool::snap, "SNAP", "N", "Placement settings: surfaces, grid, steps, local axes."},
}};
// A tool on the rail. Its panel's tool (PLACE, SETS, SCENE, SNAP) is blue while that panel shows;
// SELECT and the gizmo tools are white while they are what a click does.
bool rail_button(const ParkEditorUI &ui, const RailEntry &entry, ImVec2 size, Chip lit, bool enabled, unsigned seed) {
    const float k = ui.ui_scale;
    const auto at = ImGui::GetCursorScreenPos();
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(entry.name, size);
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    auto *draw = ImGui::GetWindowDrawList();
    ImU32 fill = hovered ? skate_theme::tile_grey : 0, ink = IM_COL32(185, 187, 192, 255),
          key = IM_COL32(109, 111, 116, 255);
    if (lit == Chip::blue) {
        fill = hovered ? skate_theme::blue_hover : blue;
        ink = skate_theme::black;
        key = IM_COL32(0, 48, 100, 255);
    } else if (lit == Chip::white) {
        fill = IM_COL32(226, 227, 230, 255);
        ink = skate_theme::black;
        key = IM_COL32(90, 92, 96, 255);
    }
    if (!enabled)
        ink = key = IM_COL32(70, 72, 76, 255);
    if (fill)
        theme::tile_rect(draw, at, plus(at, size), fill, seed, k);
    ImFont *bold = font_or(ui.bold_font);
    const auto name = bold->CalcTextSizeA(13 * k, FLT_MAX, 0, entry.name);
    const auto letter = ImGui::GetFont()->CalcTextSizeA(11 * k, FLT_MAX, 0, entry.key);
    const float top = at.y + (size.y - name.y - letter.y - 2 * k) * .5f;
    draw->AddText(bold, 13 * k, {at.x + (size.x - name.x) * .5f, top}, ink, entry.name);
    draw->AddText(ImGui::GetFont(), 11 * k, {at.x + (size.x - letter.x) * .5f, top + name.y + 2 * k}, key, entry.key);
    if (hovered)
        ImGui::SetTooltip("%s (%s)", entry.hint, entry.key);
    return pressed;
}
} // namespace

void rail_layout(ParkEditorUI &ui, EditorFrame &f) {
    const auto &model = f.model;
    const auto &state = model.editor;
    const auto &callbacks = f.callbacks;
    const float k = f.k;
    const auto P = [k](float value) { return value * k; };
    const ImVec2 o = f.origin, s = f.size;
    auto *frame = ImGui::GetWindowDrawList();
    ImFont *title_font = font_or(ui.title_font);
    const float top = P(56), bottom = P(38), rail = P(76);
    const float panel = std::clamp(s.x * .22f, P(250), P(340)), inspector = std::clamp(s.x * .22f, P(250), P(320));
    const float body_y = o.y + top, body = std::max(P(100), s.y - top - bottom);
    const ImVec2 view_at{o.x + rail + panel, body_y};
    const ImVec2 view_size{std::max(P(20), s.x - rail - panel - inspector), body};

    // ---- top bar: PARK EDITOR, the map, the project picker; history, save, test, layout, exit
    frame->AddRectFilled(o, {o.x + s.x, o.y + top}, IM_COL32(11, 11, 11, 250));
    frame->AddRectFilled({o.x, o.y + top - P(2)}, {o.x + s.x, o.y + top}, IM_COL32(31, 31, 33, 255));
    float x = o.x + P(18);
    {
        const int start = frame->VtxBuffer.Size;
        const auto extent = title_font->CalcTextSizeA(P(30), FLT_MAX, 0, "PARK EDITOR");
        const ImVec2 at{x, o.y + (top - extent.y) * .5f};
        frame->AddText(title_font, P(30), {at.x + P(2), at.y + P(2)}, IM_COL32(0, 0, 0, 160), "PARK EDITOR");
        frame->AddText(title_font, P(30), at, paper, "PARK EDITOR");
        skate_theme::rotate_since(frame, start, -3.0f, {at.x + extent.x * .5f, at.y + extent.y * .5f});
        x += extent.x + P(14);
        const char *map = state.map.empty() ? "Build your own park." : state.map.c_str();
        frame->AddText({x, o.y + (top - ImGui::GetTextLineHeight()) * .5f}, muted, map);
        x += ImGui::CalcTextSize(map).x + P(16);
    }
    const float chip_height = P(36), chip_y = o.y + (top - chip_height) * .5f, chip_gap = P(6);
    const std::array<const char *, 6> actions{"UNDO", "REDO", "SAVE", "TEST PARK", "LAYOUT", "EXIT"};
    float actions_width = chip_gap * static_cast<float>(actions.size() - 1);
    for (const auto *label : actions)
        actions_width += chip_width(ui, label);
    const float actions_x = o.x + s.x - P(18) - actions_width;
    const char *autosave = "Layout autosaves";
    const float autosave_width = ImGui::CalcTextSize(autosave).x;
    // The project picker takes what is left between the map and the actions.
    const auto name = project_name(model), version = project_version(model);
    const auto picker = "Project: " + (name.empty() ? std::string("unsaved") : name) +
                        (version.empty() ? std::string() : "  " + std::string(middot) + "  v" + version);
    const bool show_autosave = actions_x - x > P(200) + autosave_width + P(24);
    const float picker_room = actions_x - x - P(16) - (show_autosave ? autosave_width + P(24) : 0);
    const float picker_width = std::clamp(ImGui::CalcTextSize(picker.c_str()).x + P(44), P(60), std::max(P(60), picker_room));
    ImGui::SetCursorScreenPos({x, o.y + (top - P(32)) * .5f});
    ImGui::BeginDisabled(f.busy);
    const bool open = ImGui::InvisibleButton("##project", {picker_width, P(32)});
    const bool picker_hovered = ImGui::IsItemHovered();
    ImGui::EndDisabled();
    if (open)
        open_project_dialog(ui, model);
    if (picker_hovered)
        ImGui::SetTooltip("Create, open or save a park mod.");
    {
        const auto a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        theme::tile_rect(frame, a, b, picker_hovered ? skate_theme::tile_light : IM_COL32(29, 29, 31, 255), 301, k);
        frame->PushClipRect(a, {b.x - P(24), b.y}, true);
        frame->AddText({a.x + P(12), a.y + (P(32) - ImGui::GetTextLineHeight()) * .5f}, paper, picker.c_str());
        frame->PopClipRect();
        const ImVec2 c{b.x - P(14), (a.y + b.y) * .5f};
        frame->AddTriangleFilled({c.x - 4 * k, c.y - 2 * k}, {c.x + 4 * k, c.y - 2 * k}, {c.x, c.y + 3 * k}, paper);
    }
    if (show_autosave)
        frame->AddText({actions_x - P(18) - autosave_width, o.y + (top - ImGui::GetTextLineHeight()) * .5f}, muted,
                       autosave);
    ImGui::SetCursorScreenPos({actions_x, chip_y});
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {chip_gap, 0});
    ImGui::BeginDisabled(f.busy);
    if (chip(ui, "UNDO", Chip::plain, chip_height, 311, state.can_undo))
        send(ui, model, callbacks, "undo");
    ImGui::SameLine();
    if (chip(ui, "REDO", Chip::plain, chip_height, 312, state.can_redo))
        send(ui, model, callbacks, "redo");
    ImGui::SameLine();
    if (chip(ui, "SAVE", Chip::tile, chip_height, 313))
        save_project(ui, model, callbacks);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(name.empty() ? "Save the layout as a park mod (Ctrl+S)." : "Save into %s (Ctrl+S).", name.c_str());
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(state.busy || ui.pending || ui.preview_token);
    if (chip(ui, "TEST PARK", Chip::blue, chip_height, 314))
        leave(ui, f, true);
    ImGui::SameLine();
    if (chip(ui, "LAYOUT", Chip::plain, chip_height, 315))
        set_layout(ui, callbacks, park_editor_layout_hotbar);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Switch to the hotbar: the world fills the screen, the objects along the bottom.");
    ImGui::SameLine();
    if (chip(ui, "EXIT", Chip::plain, chip_height, 316))
        leave(ui, f, false);
    ImGui::EndDisabled();
    ImGui::PopStyleVar();

    // ---- the rail of tools
    frame->AddRectFilled({o.x, body_y}, {o.x + rail, body_y + body}, IM_COL32(11, 11, 11, 250));
    {
        const float spacing = P(6);
        const float button = std::min(P(60), (body - P(12)) / static_cast<float>(rail_tools.size()) - spacing);
        float y = body_y + P(12);
        const bool placing = !ui.placing.empty() || !ui.placing_set.empty();
        for (std::size_t i = 0; i < rail_tools.size(); ++i) {
            const auto &entry = rail_tools[i];
            Chip lit = Chip::plain;
            bool enabled = true;
            switch (entry.tool) {
            case RailTool::place:
                lit = ui.rail_panel == ParkRailPanel::place ? Chip::blue : Chip::plain;
                break;
            case RailTool::select:
                lit = placing ? Chip::plain : Chip::white;
                break;
            case RailTool::move:
            case RailTool::rotate:
            case RailTool::scale: {
                const auto mode = entry.tool == RailTool::move     ? ParkTransformMode::move
                                  : entry.tool == RailTool::rotate ? ParkTransformMode::rotate
                                                                   : ParkTransformMode::scale;
                lit = ui.mode == mode ? Chip::white : Chip::plain;
                enabled = !f.busy && (mode != ParkTransformMode::scale || ui.scaling_allowed);
                break;
            }
            case RailTool::sets:
                lit = ui.rail_panel == ParkRailPanel::sets ? Chip::blue : Chip::plain;
                break;
            case RailTool::scene:
                lit = ui.rail_panel == ParkRailPanel::scene ? Chip::blue : Chip::plain;
                break;
            case RailTool::snap:
                lit = ui.rail_panel == ParkRailPanel::snap ? Chip::blue : Chip::plain;
                break;
            }
            ImGui::SetCursorScreenPos({o.x + (rail - button) * .5f, y});
            if (rail_button(ui, entry, {button, button}, lit, enabled, static_cast<unsigned>(321 + i))) {
                switch (entry.tool) {
                case RailTool::place:
                    set_rail_panel(ui, ParkRailPanel::place);
                    break;
                case RailTool::select:
                    stop_placing(ui, model);
                    break;
                case RailTool::move:
                    set_mode(ui, ParkTransformMode::move);
                    break;
                case RailTool::rotate:
                    set_mode(ui, ParkTransformMode::rotate);
                    break;
                case RailTool::scale:
                    set_mode(ui, ParkTransformMode::scale);
                    break;
                case RailTool::sets:
                    set_rail_panel(ui, ParkRailPanel::sets);
                    break;
                case RailTool::scene:
                    set_rail_panel(ui, ParkRailPanel::scene);
                    break;
                case RailTool::snap:
                    set_rail_panel(ui, ParkRailPanel::snap);
                    break;
                }
            }
            y += button + spacing;
        }
    }

    // ---- the panel beside the rail, following its tool
    float_panel("rail-panel", {o.x + rail, body_y}, {panel, body}, 331, k, {P(16), P(14)}, [&] {
        switch (ui.rail_panel) {
        case ParkRailPanel::place:
            heading(ui, "PLACE");
            library_search(ui, model, -1);
            library_chip_row(ui, model, false, true, P(26));
            ImGui::Spacing();
            object_grid(ui, model, 3);
            break;
        case ParkRailPanel::sets:
            heading(ui, "SETS");
            set_list(ui, model);
            break;
        case ParkRailPanel::scene:
            heading(ui, ("SCENE  " + std::string(middot) + "  " + std::to_string(state.objects.size())).c_str());
            library_search(ui, model, -1);
            scene_list(ui, model, true);
            break;
        case ParkRailPanel::snap:
            heading(ui, "SNAP");
            placement_settings(ui);
            break;
        }
    });

    // ---- the inspector: the selection, its transform, and the scene under it
    float_panel("rail-inspector", {o.x + s.x - inspector, body_y}, {inspector, body}, 332, k, {P(16), P(14)}, [&] {
        heading(ui, selection_title(ui, model).c_str());
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", selection_detail(ui, model).c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        transform_fields(ui, model, callbacks);
        if (ui.rail_panel != ParkRailPanel::scene) { // the panel lists the scene already
            ImGui::Spacing();
            heading(ui, ("SCENE  " + std::string(middot) + "  " + std::to_string(state.objects.size())).c_str());
            scene_list(ui, model, false);
        }
    });

    // ---- the world between them, and its switches at its top left
    const float line = ImGui::GetTextLineHeightWithSpacing();
    viewport_area(ui, f, "rail-viewport", view_at, view_size,
                  {view_at.x + P(16), view_at.y + view_size.y - P(14) - line * 2}, true);
    {
        const auto snap = snap_label(ui);
        const auto plane = std::format("GRID PLANE Y {:.1f}", ui.plane_height);
        const float height = P(30), spacing = P(4);
        const float width = chip_width(ui, snap.c_str(), 14) + chip_width(ui, plane.c_str(), 14) +
                            chip_width(ui, "LOCAL AXES", 14) + spacing * 2 + P(8);
        float_panel("viewport-switches", {view_at.x + P(16), view_at.y + P(14)}, {width, height + P(8)}, 341, k,
                    {P(4), P(4)}, [&] {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {spacing, 0});
            if (chip(ui, (snap + "##snap").c_str(), ui.snapping ? Chip::white : Chip::plain, height, 342, true, 0, 14))
                ui.snapping = !ui.snapping;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Snap to the grid and the angle step. SNAP on the rail sets them.");
            ImGui::SameLine();
            if (chip(ui, (plane + "##plane").c_str(), ui.surface_mode ? Chip::plain : Chip::white, height, 343, true, 0, 14)) {
                ui.surface_mode = !ui.surface_mode;
                ui.surface_confirm = false;
                ui.surface_valid = false;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(ui.surface_mode ? "Objects land on the game's collision. Click to build on the grid plane."
                                                  : "Objects land on the grid plane. Click to place on surfaces.");
            ImGui::SameLine();
            if (chip(ui, "LOCAL AXES", ui.local_axes ? Chip::white : Chip::plain, height, 344, true, 0, 14))
                ui.local_axes = !ui.local_axes;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Move and rotate along the object's own axes.");
            ImGui::PopStyleVar();
        }, bar_colour);
    }

    // ---- status bar
    static constexpr std::array<Keycap, 5> keys{{{"RMB", "Fly"},
                                                 {"WASD", "Move"},
                                                 {"T R Y", "Move / rotate / scale"},
                                                 {"CTRL Z", "Undo"},
                                                 {"ESC", "Close"}}};
    status_line(ui, f, {o.x, o.y + s.y - bottom}, {s.x, bottom}, keys, false);
}
} // namespace dingosdk::overlay::park_editor_detail
