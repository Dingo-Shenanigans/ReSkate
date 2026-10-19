#include "park_editor_internal.h"
#include "park_previews.h"
#include <algorithm>
#include <set>

namespace dingosdk::overlay::park_editor_detail {
using namespace editor;
namespace {
// A section header like the menu's: heading text over a rule with a blue stub.
void heading(const ParkEditorUI &ui, const char *text) {
    const float k = ui.ui_scale;
    ImGui::PushFont(font_or(ui.heading_font));
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const auto at = ImGui::GetCursorScreenPos();
    auto *draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, {at.x + ImGui::GetContentRegionAvail().x, at.y + 2 * k}, skate_theme::tile_light);
    draw->AddRectFilled(at, {at.x + 34 * k, at.y + 3 * k}, blue);
    ImGui::Dummy({0, 8 * k});
}
bool matches(std::string value, std::string filter) {
    const auto lower = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
    };
    std::transform(value.begin(), value.end(), value.begin(), lower);
    std::transform(filter.begin(), filter.end(), filter.begin(), lower);
    return value.find(filter) != std::string::npos;
}
std::string card_title(const std::string &title, float width) {
    std::string result;
    const char *start = title.data(), *end = start + title.size();
    auto *font = ImGui::GetFont();
    for (unsigned line = 0; line < 3 && start < end; ++line) {
        if (line)
            result += '\n';
        if (line < 2) {
            const auto *wrap =
                font->CalcWordWrapPositionA(ImGui::GetFontSize() / font->FontSize, start, end, width);
            if (wrap <= start)
                break;
            result.append(start, wrap);
            start = wrap;
            while (start < end && (*start == ' ' || *start == '\t' || *start == '\n'))
                ++start;
        } else {
            std::string tail(start, end);
            if (ImGui::CalcTextSize(tail.c_str()).x > width) {
                do {
                    auto last = tail.size() - 1;
                    while (last && (static_cast<unsigned char>(tail[last]) & 0xc0) == 0x80)
                        --last;
                    tail.resize(last);
                } while (!tail.empty() && ImGui::CalcTextSize((tail + "...").c_str()).x > width);
                tail += "...";
            }
            result += tail;
        }
    }
    return result;
}
// OBJECTS / SETS: which list the library shows.
void library_tabs(ParkEditorUI &ui) {
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
    ImGui::PushFont(font_or(ui.bold_font));
    for (const bool sets : {false, true}) {
        if (sets)
            ImGui::SameLine();
        const bool on = ui.library_sets == sets;
        if (on)
            skate_theme::push_primary_button();
        if (ImGui::Button(sets ? "SETS" : "OBJECTS", {sets ? -1 : half, 0}))
            ui.library_sets = sets;
        if (on)
            skate_theme::pop_primary_button();
    }
    ImGui::PopFont();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Groups of objects you saved, to place again anywhere.");
}
void object_sets(ParkEditorUI &ui, const Model &model) {
    const float k = ui.ui_scale;
    const auto &state = model.editor;
    const bool busy = state.busy || state.failed || ui.pending || ui.preview_token || !state.available;
    const auto &sets = state.object_sets;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Select objects (Shift-click adds), name them and save them as a set. "
                        "Place it again anywhere, on any map.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint("##set-name", "Set name", ui.set_name.data(), ui.set_name.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    const auto count = ui.selection.size();
    const bool can_save = !busy && count && ui.set_name[0];
    ImGui::BeginDisabled(!can_save);
    skate_theme::push_primary_button();
    ImGui::PushFont(font_or(ui.bold_font));
    const auto label = count ? "SAVE " + std::to_string(count) + (count == 1 ? " OBJECT" : " OBJECTS") + " AS SET"
                             : std::string("SELECT OBJECTS TO SAVE");
    const bool save = ImGui::Button(label.c_str(), {-1, 34 * k});
    ImGui::PopFont();
    skate_theme::pop_primary_button();
    ImGui::EndDisabled();
    if ((save || enter) && can_save)
        save_set(ui, state);
    const std::string typed = ui.set_name.data();
    if (sets && !typed.empty() &&
        std::any_of(sets->begin(), sets->end(), [&](const auto &set) { return set.name == typed; }))
        ImGui::TextDisabled("Saving replaces the set with this name.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##set-search", "Search sets...", ui.search.data(), ui.search.size());
    std::vector<const EditorObjectSet *> filtered;
    if (sets)
        for (const auto &set : *sets)
            if (matches(set.name + " " + set.map, ui.search.data()))
                filtered.push_back(&set);
    ImGui::TextDisabled("%zu sets  /  Place, then click in the world", filtered.size());
    ImGui::Spacing();
    ImGui::BeginChild("set-list", {0, 0});
    if (filtered.empty())
        ImGui::TextDisabled(sets && !sets->empty() ? "No set matches the search." : "No saved sets yet.");
    for (std::size_t index = 0; index < filtered.size(); ++index) {
        const auto &set = *filtered[index];
        ImGui::PushID(set.name.c_str());
        auto *draw = ImGui::GetWindowDrawList();
        const auto top = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetTextLineHeight() * 2 + ImGui::GetFrameHeight() + 26 * k;
        const bool chosen = ui.placing_set == set.name;
        skate_theme::rough_rect(draw, top, plus(top, {width, height}),
                                chosen ? skate_theme::tile_light : skate_theme::tile_grey,
                                static_cast<unsigned>(index + 91), k);
        if (chosen)
            draw->AddRect(plus(top, {-1, -1}), plus(top, {width + 1, height + 1}), blue, 0, 0, 3 * k);
        std::size_t missing = 0;
        if (state.assets)
            for (const auto &object : set.objects)
                missing += std::none_of(state.assets->begin(), state.assets->end(),
                                        [&](const auto &asset) { return asset.key == object.item; });
        ImGui::SetCursorScreenPos(plus(top, {10 * k, 8 * k}));
        ImGui::BeginGroup();
        ImGui::PushFont(font_or(ui.bold_font));
        ImGui::TextUnformatted(set.name.c_str());
        ImGui::PopFont();
        if (missing)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(skate_theme::danger), "%zu objects  /  %zu unavailable",
                               set.objects.size(), missing);
        else
            ImGui::TextDisabled("%zu objects  /  saved on %s", set.objects.size(), set.map.c_str());
        const float buttons = (width - 20 * k - ImGui::GetStyle().ItemSpacing.x) * .5f;
        ImGui::BeginDisabled(busy || missing);
        if (ImGui::Button(chosen ? "Placing..." : "Place", {buttons, 0})) {
            if (chosen)
                ui.placing_set.clear();
            else
                begin_set_placement(ui, model, set.name);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(busy);
        const bool confirming = ui.set_delete == set.name;
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(skate_theme::danger));
        if (ImGui::Button(confirming ? "Really delete?" : "Delete", {buttons, 0})) {
            if (confirming)
                delete_set(ui, state, set.name);
            else
                ui.set_delete = set.name;
        }
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(plus(top, {0, height + 6 * k}));
        ImGui::Dummy({width, 0});
        ImGui::PopID();
    }
    ImGui::EndChild();
}
} // namespace
void library(ParkEditorUI &ui, const Model &model) {
    const float k = ui.ui_scale;
    heading(ui, "OBJECT LIBRARY");
    library_tabs(ui);
    if (ui.library_sets) {
        object_sets(ui, model);
        return;
    }
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##library-search", "Search objects...", ui.search.data(), ui.search.size());
    const auto &assets = model.editor.assets;
    std::set<std::string> categories;
    if (assets)
        for (const auto &item : *assets)
            categories.insert(item.category);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##category", ui.category.empty() ? "All categories" : ui.category.c_str())) {
        if (ImGui::Selectable("All categories", ui.category.empty()))
            ui.category.clear();
        for (const auto &category : categories)
            if (ImGui::Selectable(category.c_str(), ui.category == category))
                ui.category = category;
        ImGui::EndCombo();
    }
    std::vector<const EditorAsset *> filtered;
    if (assets)
        for (const auto &item : *assets)
            if ((ui.category.empty() || ui.category == item.category) &&
                matches(item.title + " " + item.key, ui.search.data()))
                filtered.push_back(&item);
    ImGui::TextDisabled("%zu objects  /  click or drag to place", filtered.size());
    ImGui::Spacing();
    ImGui::BeginChild("library-grid", {0, 0});
    const int columns = std::max(2, static_cast<int>(ImGui::GetContentRegionAvail().x / (120 * k)));
    const float card_width = ImGui::GetContentRegionAvail().x / static_cast<float>(columns) - 8;
    const float card_height = card_width + ImGui::GetTextLineHeight() * 3 + 12;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4, 4});
    if (ImGui::BeginTable("asset-cards", columns,
                          ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ImGuiListClipper clipper;
        clipper.Begin((static_cast<int>(filtered.size()) + columns - 1) / columns, card_height + 8);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                ImGui::TableNextRow(0, card_height + 8);
                for (int column = 0; column < columns; ++column) {
                    const int index = row * columns + column;
                    if (index >= static_cast<int>(filtered.size()))
                        break;
                    ImGui::TableSetColumnIndex(column);
                    const auto &item = *filtered[static_cast<std::size_t>(index)];
                    ImGui::PushID(item.key.c_str());
                    const auto top = ImGui::GetCursorScreenPos();
                    const bool clicked = ImGui::InvisibleButton("##asset", {card_width, card_height});
                    const bool hovered = ImGui::IsItemHovered();
                    if (clicked) {
                        if (ui.preview_token)
                            cancel_drag(ui, model);
                        ui.placing = item.key;
                        ui.placing_set.clear();
                        ui.selected = 0;
                        ui.selection.clear();
                        ui.angles = {};
                        ui.scale = 1;
                        ui.inspector_dirty = false;
                        ui.surface_confirm = false;
                    }
                    auto *draw = ImGui::GetWindowDrawList();
                    const bool chosen = ui.placing == item.key;
                    skate_theme::rough_rect(draw, top, plus(top, {card_width, card_height}),
                                            chosen || hovered ? skate_theme::tile_light : skate_theme::tile_grey,
                                            static_cast<unsigned>(index + 7), k);
                    if (chosen)
                        draw->AddRect(plus(top, {-1, -1}), plus(top, {card_width + 1, card_height + 1}), blue, 0, 0,
                                      3 * k);
                    ParkPreviewImage image;
                    const bool has_preview = park_preview_image(item.key, image);
                    if (has_preview)
                        draw->AddImage(image.texture, plus(top, {6, 6}),
                                       plus(top, {card_width - 6, card_width - 6}), image.uv0, image.uv1);
                    else {
                        const char *label = "No preview";
                        const auto extent = ImGui::CalcTextSize(label);
                        draw->AddText(
                            plus(top, {(card_width - extent.x) * .5f, card_width * .5f - extent.y * .5f}),
                            muted, label);
                    }
                    draw->PushClipRect(plus(top, {7, card_width}),
                                       plus(top, {card_width - 7, card_height - 5}), true);
                    const auto title = card_title(item.title, card_width - 14);
                    draw->AddText(plus(top, {7, card_width}), paper, title.c_str());
                    draw->PopClipRect();
                    if (ImGui::BeginDragDropSource()) {
                        ui.asset_drag = true;
                        ImGui::SetDragDropPayload("PARK_ASSET", item.key.c_str(), item.key.size() + 1);
                        ImGui::TextUnformatted(item.title.c_str());
                        if (has_preview)
                            ImGui::Image(image.texture, {112, 112}, image.uv0, image.uv1);
                        ImGui::EndDragDropSource();
                    }
                    if (hovered && !ImGui::GetDragDropPayload()) {
                        ImGui::BeginTooltip();
                        if (has_preview)
                            ImGui::Image(image.texture, {192, 192}, image.uv0, image.uv1);
                        ImGui::TextUnformatted(item.title.c_str());
                        ImGui::TextDisabled("%s", item.category.c_str());
                        ImGui::TextDisabled("%s", item.key.c_str());
                        ImGui::EndTooltip();
                    }
                    ImGui::PopID();
                }
            }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}
void inspector(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks) {
    const float k = ui.ui_scale;
    heading(ui, "TRANSFORM");
    const auto *selected = selected_object(ui, model.editor);
    const bool editable = selected || !ui.placing.empty() || !ui.placing_set.empty();
    if (!ui.placing_set.empty())
        ImGui::TextDisabled("Placing the set %s", ui.placing_set.c_str());
    else if (ui.selection.size() > 1)
        ImGui::TextDisabled("%zu objects / shared pivot", ui.selection.size());
    else if (selected)
        ImGui::TextDisabled("Object %llu", static_cast<unsigned long long>(selected->id));
    else
        ImGui::TextDisabled(ui.placing.empty() ? "Select an object" : "New object placement");
    const bool own_placement =
        ui.preview_token && ui.preview_place && model.editor.preview_token == ui.preview_token;
    ImGui::BeginDisabled(!editable || ui.pending || (model.editor.busy && !own_placement) ||
                         (ui.preview_token && !own_placement));
    ImGui::TextUnformatted("Position");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat3("##position", ui.position.data(), .1f, -100000, 100000, "%.3f"))
        ui.inspector_dirty = true;
    ImGui::TextUnformatted("Rotation (degrees)");
    ImGui::SetNextItemWidth(-1);
    if (ImGui::DragFloat3("##rotation", ui.angles.data(), 1, -36000, 36000, "%.1f"))
        ui.inspector_dirty = true;
    ImGui::TextUnformatted(ui.selection.size() > 1 ? "Scale multiplier" : "Uniform scale");
    ImGui::SetNextItemWidth(-1);
    ImGui::BeginDisabled(!ui.scaling_allowed);
    if (ImGui::DragFloat("##scale", &ui.scale, .01f, .01f, 100, "%.3f",
                         ImGuiSliderFlags_AlwaysClamp))
        ui.inspector_dirty = true;
    ImGui::EndDisabled();
    if (!ui.scaling_allowed) ImGui::TextDisabled("This server has object scaling off.");
    ImGui::BeginDisabled(!selected && ui.surface_mode && !ui.surface_valid && !ui.inspector_dirty);
    skate_theme::push_primary_button();
    ImGui::PushFont(font_or(ui.bold_font));
    const bool apply = ImGui::Button(selected                    ? "APPLY TRANSFORM"
                                     : !ui.placing_set.empty() ? "PLACE SET AT COORDINATES"
                                                               : "PLACE AT COORDINATES",
                                     {-1, 34 * k});
    ImGui::PopFont();
    skate_theme::pop_primary_button();
    if (apply) {
        if (ui.snapping) {
            if (selected || !ui.surface_mode || ui.inspector_dirty)
                for (auto &v : ui.position)
                    v = snapped(v, ui.grid);
            // A group's own rotation is not on the angle grid: snap only a rotation typed for it.
            if (ui.selection.size() <= 1 || ui.angles != ui.group_angles)
                for (auto &v : ui.angles)
                    v = snapped(v, ui.angle_snap);
        }
        bool applied{};
        if (!ui.placing_set.empty())
            applied = place_set(ui, model.editor);
        else if (ui.selection.size() > 1) {
            const auto position = ui.position, angles = ui.angles;
            const auto scale = ui.scale;
            refresh_selection(ui, model.editor);
            capture_drag(ui, model.editor);
            ui.position = position;
            ui.angles = angles;
            ui.scale = scale;
            applied = preview(ui, model, EditorPreviewAction::begin_move) &&
                      preview(ui, model, EditorPreviewAction::commit);
        } else if (ui.preview_token)
            applied = preview(ui, model, EditorPreviewAction::commit);
        else
            applied = send(ui, model, callbacks, selected ? "move" : "place", ui.placing);
        if (applied)
            ui.placing.clear();
    }
    ImGui::EndDisabled();
    if (selected) {
        const bool group = ui.selection.size() > 1;
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
        if (ImGui::Button(group ? "Duplicate all" : "Duplicate", {half, 0}))
            duplicate_selection(ui, model.editor);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy %s with %s rotation and scale, one grid step along X (Ctrl+D).",
                              group ? "every selected object" : "the object", group ? "their" : "its");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(skate_theme::danger));
        const auto label = group ? "Delete " + std::to_string(ui.selection.size()) : std::string("Delete");
        if (ImGui::Button(label.c_str(), {-1, 0})) {
            if (group)
                delete_selection(ui, model.editor);
            else
                send(ui, model, callbacks, "delete");
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    heading(ui, "PLACEMENT");
    if (ImGui::Checkbox("Place on surfaces", &ui.surface_mode)) {
        ui.surface_confirm = false;
        ui.surface_valid = false;
    }
    ImGui::Checkbox("Snap to grid / angles", &ui.snapping);
    ImGui::SetNextItemWidth(90 * k);
    ImGui::DragFloat("Grid (m)", &ui.grid, .05f, .05f, 100, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(90 * k);
    ImGui::DragFloat("Angle step", &ui.angle_snap, 1, 1, 90, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(90 * k);
    ImGui::DragFloat("Scale step", &ui.scale_snap, .01f, .01f, 10, "%.2f",
                     ImGuiSliderFlags_AlwaysClamp);
    if (ui.surface_mode) {
        ImGui::SetNextItemWidth(90 * k);
        ImGui::DragFloat("Lift (m)", &ui.surface_offset, .05f, -100, 100, "%.2f",
                         ImGuiSliderFlags_AlwaysClamp);
    } else {
        ImGui::SetNextItemWidth(90 * k);
        ImGui::DragFloat("Build plane Y", &ui.plane_height, .1f, -10000, 10000, "%.2f",
                         ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetNextItemWidth(90 * k);
        ImGui::DragFloat("Ray distance", &ui.distance, .5f, 1, 500, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(ui.surface_mode ? "Placement follows game collision."
                                        : "Placement uses the grid plane.");
    ImGui::TextDisabled("Wire cube marks the object origin.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    heading(ui, ("SCENE  /  " + std::to_string(model.editor.objects.size()) + " OBJECTS").c_str());
    ImGui::BeginChild("scene-list", {0, 0});
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(model.editor.objects.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto &row = model.editor.objects[static_cast<std::size_t>(i)];
            std::string title = row.item;
            const auto &assets = model.editor.assets;
            if (assets) {
                const auto it = std::find_if(assets->begin(), assets->end(),
                                             [&](const auto &a) { return a.key == row.item; });
                if (it != assets->end())
                    title = it->title;
            }
            const auto label = title + "##" + std::to_string(row.id);
            ImGui::BeginDisabled(ui.preview_token || ui.pending || model.editor.busy);
            if (ImGui::Selectable(label.c_str(), std::find(ui.selection.begin(), ui.selection.end(),
                                                           row.id) != ui.selection.end()))
                choose(ui, model.editor, row, ImGui::GetIO().KeyShift);
            ImGui::EndDisabled();
        }
    ImGui::EndChild();
}
} // namespace dingosdk::overlay::park_editor_detail
