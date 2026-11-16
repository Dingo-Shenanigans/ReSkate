#include "park_editor_internal.h"
#include <algorithm>
#include <array>

// The hotbar layout: the world fills the screen, with dark bars floating over it. Along the top the
// title, the tools and the file actions; along the bottom the object library as a strip of cards and
// the status line; on the right the selection's card while there is one.
namespace dingosdk::overlay::park_editor_detail {
namespace {
constexpr const char *middot = "\xc2\xb7";
// A small arrow pointing down at `centre`, for a chip that opens something under it.
void down_arrow(ImDrawList *draw, ImVec2 centre, float k, ImU32 colour) {
    draw->AddTriangleFilled({centre.x - 4 * k, centre.y - 2 * k}, {centre.x + 4 * k, centre.y - 2 * k},
                            {centre.x, centre.y + 3 * k}, colour);
}
struct ToolChip {
    const char *label;
    ParkTransformMode mode;
};
constexpr std::array<ToolChip, 3> tools{{{"MOVE", ParkTransformMode::move},
                                         {"ROTATE", ParkTransformMode::rotate},
                                         {"SCALE", ParkTransformMode::scale}}};
} // namespace

void hotbar_layout(ParkEditorUI &ui, EditorFrame &f) {
    const auto &model = f.model;
    const auto &state = model.editor;
    const auto &callbacks = f.callbacks;
    const float k = f.k;
    const auto P = [k](float value) { return value * k; };
    const ImVec2 o = f.origin, s = f.size;
    ImFont *title_font = font_or(ui.title_font);
    const float margin = P(16), gap = P(10), bar = P(52), chip_height = P(36), pad = P(8), chip_gap = P(4);

    // ---- measure the three top bars, then lay them out on one row or, when narrow, two
    const auto name = project_name(model);
    const bool unsaved = state.project.empty();
    const float title_width = title_font->CalcTextSizeA(P(30), FLT_MAX, 0, "BUILD").x;
    const auto place = (name.empty() ? std::string("New park") : name) + "  " + middot + "  " +
                       (state.map.empty() ? std::string("Build your own park.") : state.map);
    const float tag_width = chip_width(ui, "UNSAVED", 12) - P(12);
    float place_width = std::min(ImGui::CalcTextSize(place.c_str()).x, P(320));
    const auto title_bar_width = [&] {
        return P(18) + title_width + P(14) + place_width + P(16) + (unsaved ? P(12) + tag_width : 0) + P(16);
    };
    const auto snap = snap_label(ui);
    // The tools, the divider (6 + 2 + 6), SNAP and its arrow, ON SURFACES, as the bar draws them.
    float tool_bar = pad * 2 + P(14) + chip_width(ui, snap.c_str()) + P(28) + chip_gap + chip_width(ui, "ON SURFACES");
    int tool_count = 0;
    for (const auto &tool : tools) {
        tool_bar += chip_width(ui, tool.label) + (tool_count ? chip_gap : 0);
        ++tool_count;
    }
    const std::array<const char *, 6> actions{"UNDO", "REDO", "SAVE", "TEST PARK", "LAYOUT", "EXIT"};
    float action_bar = pad * 2 + chip_gap * static_cast<float>(actions.size() - 1);
    for (const auto *label : actions)
        action_bar += chip_width(ui, label);
    const float room = s.x - margin * 2;
    bool two_rows = title_bar_width() + tool_bar + action_bar + gap * 2 > room;
    if (two_rows)
        place_width = std::clamp(room - action_bar - gap - (title_bar_width() - place_width), P(60), place_width);
    const float title_bar = title_bar_width();
    const ImVec2 title_at{o.x + margin, o.y + margin};
    const ImVec2 action_at{o.x + s.x - margin - action_bar, o.y + margin};
    const ImVec2 tool_at = two_rows ? ImVec2{o.x + margin, o.y + margin + bar + gap}
                                    : ImVec2{title_at.x + title_bar + gap, o.y + margin};
    const float top_bottom = tool_at.y + bar;

    // ---- the bottom: the status line, the library strip above it
    const float status_height = P(34), status_y = o.y + s.y - P(8) - status_height;
    const float card = std::clamp(s.y * .15f, P(96), P(150));
    const float strip_height = P(12) * 2 + P(30) + P(8) + card + P(42) + ImGui::GetStyle().ScrollbarSize;
    const bool library = !ui.library_hidden;
    const float strip_y = status_y - P(6) - strip_height;
    const float floor_y = library ? strip_y : status_y; // what the floating panels stay above

    // ---- the world, under everything that floats
    viewport_area(ui, f, "hotbar-viewport", o, s,
                  {o.x + margin + P(4), floor_y - ImGui::GetTextLineHeightWithSpacing() - P(8)}, false);

    // ---- title bar: BUILD, the park and map (opens PARK PROJECT), UNSAVED
    float_panel("title-bar", title_at, {title_bar, bar}, 201, k, {0, 0}, [&] {
        auto *draw = ImGui::GetWindowDrawList();
        const auto at = ImGui::GetWindowPos();
        const int start = draw->VtxBuffer.Size;
        const auto extent = title_font->CalcTextSizeA(P(30), FLT_MAX, 0, "BUILD");
        const ImVec2 text{at.x + P(18), at.y + (bar - extent.y) * .5f};
        draw->AddText(title_font, P(30), {text.x + P(2), text.y + P(2)}, IM_COL32(0, 0, 0, 160), "BUILD");
        draw->AddText(title_font, P(30), text, paper, "BUILD");
        skate_theme::rotate_since(draw, start, -3.0f, {text.x + extent.x * .5f, text.y + extent.y * .5f});
        const float x = text.x + title_width + P(14);
        ImGui::SetCursorScreenPos({x - P(6), at.y + P(8)});
        ImGui::BeginDisabled(f.busy);
        const bool open = ImGui::InvisibleButton("##project", {place_width + P(28), bar - P(16)});
        const bool hovered = ImGui::IsItemHovered();
        ImGui::EndDisabled();
        if (hovered)
            theme::tile_rect(draw, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), skate_theme::tile_grey, 202, k);
        if (open)
            open_project_dialog(ui, model);
        if (hovered)
            ImGui::SetTooltip("Create, open or save a park mod.");
        const float line = ImGui::GetTextLineHeight();
        draw->PushClipRect({x, at.y}, {x + place_width, at.y + bar}, true);
        draw->AddText({x, at.y + (bar - line) * .5f}, hovered ? paper : muted, place.c_str());
        draw->PopClipRect();
        down_arrow(draw, {x + place_width + P(9), at.y + bar * .5f}, k, hovered ? paper : muted);
        if (unsaved) {
            const ImVec2 tag{x + place_width + P(28), at.y + (bar - P(20)) * .5f};
            ImFont *bold = font_or(ui.bold_font);
            const auto label = bold->CalcTextSizeA(P(12), FLT_MAX, 0, "UNSAVED");
            draw->AddRectFilled(tag, {tag.x + tag_width, tag.y + P(20)}, skate_theme::warning);
            draw->AddText(bold, P(12), {tag.x + (tag_width - label.x) * .5f, tag.y + (P(20) - label.y) * .5f},
                          skate_theme::black, "UNSAVED");
        }
    }, bar_colour);

    // ---- tool bar: MOVE / ROTATE / SCALE, SNAP (with its settings), ON SURFACES
    float snap_x = 0;
    float_panel("tool-bar", tool_at, {tool_bar, bar}, 203, k, {pad, (bar - chip_height) * .5f}, [&] {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {chip_gap, 0});
        ImGui::BeginDisabled(f.busy);
        bool first = true;
        for (std::size_t i = 0; i < tools.size(); ++i) {
            if (!first)
                ImGui::SameLine();
            first = false;
            // SCALE stays in the bar, greyed, while the session's host has object scaling off, so it
            // does not look as if the tool were gone (players took its absence for it being removed).
            const bool off = tools[i].mode == ParkTransformMode::scale && !ui.scaling_allowed;
            if (chip(ui, tools[i].label, ui.mode == tools[i].mode ? Chip::blue : Chip::plain, chip_height,
                     static_cast<unsigned>(41 + i), !off))
                set_mode(ui, tools[i].mode);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", off ? "Scale (Y) is off in this session: its host has turned object scaling off. It is back offline and in your own lobby." : i == 0 ? "Move along the handles (T)." : i == 1 ? "Turn on the rings (R)." : "Scale (Y).");
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, P(6));
        {
            const auto at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled({at.x, at.y + P(3)}, {at.x + P(2), at.y + chip_height - P(3)},
                                                      IM_COL32(51, 51, 51, 255));
            ImGui::Dummy({P(2), chip_height});
        }
        ImGui::SameLine(0, P(6));
        snap_x = ImGui::GetCursorScreenPos().x;
        if (chip(ui, (snap + "##snap").c_str(), ui.snapping ? Chip::white : Chip::plain, chip_height, 44))
            ui.snapping = !ui.snapping;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Snap to the grid and the angle step. The arrow beside it sets them (N).");
        ImGui::SameLine(0, 0);
        const auto arrow_at = ImGui::GetCursorScreenPos();
        if (chip(ui, "##snap-settings", ui.snap_open ? Chip::tile : Chip::plain, chip_height, 45, true, P(28)))
            ui.snap_open = !ui.snap_open;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Placement settings: grid, angle and scale steps, lift, local axes (N).");
        down_arrow(ImGui::GetWindowDrawList(), {arrow_at.x + P(14), arrow_at.y + chip_height * .5f}, k, paper);
        ImGui::SameLine();
        if (chip(ui, "ON SURFACES", ui.surface_mode ? Chip::white : Chip::plain, chip_height, 46)) {
            ui.surface_mode = !ui.surface_mode;
            ui.surface_confirm = false;
            ui.surface_valid = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(ui.surface_mode ? "Objects land on the game's collision. Off: on the grid plane."
                                              : "Objects land on the grid plane. On: on the game's collision.");
        ImGui::PopStyleVar();
    }, bar_colour);

    // ---- action bar: UNDO, REDO, SAVE, TEST PARK, LAYOUT, EXIT
    float_panel("action-bar", action_at, {action_bar, bar}, 204, k, {pad, (bar - chip_height) * .5f}, [&] {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {chip_gap, 0});
        ImGui::BeginDisabled(f.busy);
        if (chip(ui, "UNDO", Chip::plain, chip_height, 51, state.can_undo))
            send(ui, model, callbacks, "undo");
        ImGui::SameLine();
        if (chip(ui, "REDO", Chip::plain, chip_height, 52, state.can_redo))
            send(ui, model, callbacks, "redo");
        ImGui::SameLine();
        if (chip(ui, "SAVE", Chip::tile, chip_height, 53))
            save_project(ui, model, callbacks);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(unsaved ? "Save the layout as a park mod (Ctrl+S)." : "Save into %s (Ctrl+S).", name.c_str());
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(state.busy || ui.pending || ui.preview_token);
        if (chip(ui, "TEST PARK", Chip::blue, chip_height, 54))
            leave(ui, f, true);
        ImGui::SameLine();
        if (chip(ui, "LAYOUT", Chip::plain, chip_height, 55))
            set_layout(ui, callbacks, park_editor_layout_rail);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Switch to the tool rail: tools down the left, panels beside them.");
        ImGui::SameLine();
        if (chip(ui, "EXIT", Chip::plain, chip_height, 56))
            leave(ui, f, false);
        ImGui::EndDisabled();
        ImGui::PopStyleVar();
    }, bar_colour);

    // ---- the selection's card, on the right while something is selected or being placed
    if (selected_object(ui, state) || !ui.placing.empty() || !ui.placing_set.empty()) {
        const float width = std::clamp(s.x * .24f, P(280), P(340));
        const float top = top_bottom + gap;
        const float tallest = std::max(P(120), floor_y - gap - top);
        ImGui::SetNextWindowSizeConstraints({width, 0}, {width, tallest});
        float_panel("selected-card", {o.x + s.x - margin - width, top}, {width, 0}, 205, k, {P(16), P(14)}, [&] {
            const auto title = selection_title(ui, model);
            const auto count = ui.selection.size();
            const auto side = count ? std::to_string(count) + " selected" : std::string();
            const float side_width = ImGui::CalcTextSize(side.c_str()).x;
            auto *draw = ImGui::GetWindowDrawList();
            const auto at = ImGui::GetCursorScreenPos();
            const float avail = ImGui::GetContentRegionAvail().x;
            ImGui::PushFont(font_or(ui.heading_font));
            draw->PushClipRect(at, {at.x + avail - side_width - P(10), at.y + ImGui::GetTextLineHeight() + P(4)}, true);
            ImGui::TextUnformatted(title.c_str());
            draw->PopClipRect();
            ImGui::PopFont();
            if (!side.empty())
                draw->AddText({at.x + avail - side_width, at.y + P(3)}, muted, side.c_str());
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", selection_detail(ui, model).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy({0, P(4)});
            transform_fields(ui, model, callbacks);
        }, bar_colour, ImGuiChildFlags_AutoResizeY);
    }

    // ---- the library strip: chips stepped with [ and ], the search, the cards
    if (library) {
        float_panel("library-strip-panel", {o.x + margin, strip_y}, {s.x - margin * 2, strip_height}, 208, k,
                    {P(14), P(12)}, [&] {
            auto *draw = ImGui::GetWindowDrawList();
            const float row = P(30);
            const float search = std::min(P(260), ImGui::GetContentRegionAvail().x * .3f);
            const std::array<Keycap, 1> previous{{{"[", ""}}}, next{{{"]", ""}}};
            const float cap = keycaps_width(ui, previous);
            const float hide = chip_width(ui, "HIDE  H##hide", 13);
            const float chips = ImGui::GetContentRegionAvail().x - search - cap * 2 - hide - P(8) * 4;
            auto at = ImGui::GetCursorScreenPos();
            draw_keycaps(ui, draw, {at.x, at.y + (row - P(20)) * .5f}, previous);
            ImGui::Dummy({cap, row});
            ImGui::SameLine(0, P(8));
            library_chip_row(ui, model, true, false, row, std::max(P(40), chips));
            ImGui::SameLine(0, P(8));
            at = ImGui::GetCursorScreenPos();
            draw_keycaps(ui, draw, {at.x, at.y + (row - P(20)) * .5f}, next);
            ImGui::Dummy({cap, row});
            ImGui::SameLine(0, P(8));
            if (chip(ui, "HIDE  H##hide", Chip::plain, row, 209, true, 0, 13))
                ui.library_hidden = true;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hide the library to see more of the park (H brings it back).");
            ImGui::SameLine(0, P(8));
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row - ImGui::GetFrameHeight()) * .5f);
            library_search(ui, model, search);
            ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x + P(14), at.y + row + P(8)});
            switch (ui.library_view) {
            case ParkLibraryView::sets:
                set_strip(ui, model, card);
                break;
            case ParkLibraryView::scene:
                scene_strip(ui, model, card);
                break;
            default:
                object_strip(ui, model, card);
                break;
            }
        }, bar_colour);
    } else
        float_panel("library-shown", {o.x + margin, status_y - P(6) - P(40)}, {chip_width(ui, "LIBRARY  H") + P(8), P(40)},
                    210, k, {P(4), P(4)}, [&] {
            if (chip(ui, "LIBRARY  H", Chip::plain, P(32), 211))
                ui.library_hidden = false;
        }, bar_colour);

    // ---- the snap settings, under the SNAP chip and over the library while open
    if (ui.snap_open) {
        const float width = P(300);
        const float x = std::clamp(snap_x, o.x + margin, o.x + s.x - margin - width);
        ImGui::SetNextWindowSizeConstraints({width, 0}, {width, std::max(P(120), status_y - gap - top_bottom - P(6))});
        float_panel("snap-settings", {x, top_bottom + P(6)}, {width, 0}, 206, k, {P(16), P(14)}, [&] {
            heading(ui, "PLACEMENT");
            placement_settings(ui);
            ImGui::Spacing();
            if (chip(ui, "DONE##snap-done", Chip::tile, P(30), 207, true, ImGui::GetContentRegionAvail().x))
                ui.snap_open = false;
        }, IM_COL32(10, 10, 11, 255), ImGuiChildFlags_AutoResizeY); // opaque: it lies over the library
    }

    // ---- status line: what the keys do now, the latest message, the counts
    std::vector<Keycap> keys;
    if (!ui.placing_set.empty())
        keys = {{"LMB", "Place set"}, {"WHEEL", "Turn"}, {"ESC", "Cancel"}, {"RMB", "Fly"}};
    else if (!ui.placing.empty())
        keys = {{"LMB", "Place"}, {"ESC", "Cancel"}, {"RMB", "Fly"}, {"WASD", "Move"}};
    else
        keys = {{"RMB", "Fly"}, {"WASD", "Move"}, {"T R Y", "Tool"}, {"SHIFT", "Add to selection"}, {"CTRL Z", "Undo"}};
    keys.push_back({"H", ui.library_hidden ? "Show library" : "Hide library"});
    status_line(ui, f, {o.x + margin, status_y}, {s.x - margin * 2, status_height}, keys, true);
}
} // namespace dingosdk::overlay::park_editor_detail
