#include "park_editor_internal.h"
#include "park_previews.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <set>

// The pieces both layouts draw: chips, the object library, saved sets, the scene list, the
// transform fields and the placement settings. Each takes the room it is given.
namespace dingosdk::overlay::park_editor_detail {
using namespace editor;
namespace {
constexpr const char *middot = "\xc2\xb7", *degrees = "\xc2\xb0";
bool matches(std::string value, std::string filter) {
    const auto lower = [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : static_cast<char>(c);
    };
    std::transform(value.begin(), value.end(), value.begin(), lower);
    std::transform(filter.begin(), filter.end(), filter.begin(), lower);
    return value.find(filter) != std::string::npos;
}
std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}
std::string card_title(const std::string &title, float width, unsigned lines) {
    std::string result;
    const char *start = title.data(), *end = start + title.size();
    auto *font = ImGui::GetFont();
    for (unsigned line = 0; line < lines && start < end; ++line) {
        if (line)
            result += '\n';
        if (line + 1 < lines) {
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
const EditorAsset *find_asset(const Model &model, const std::string &key) {
    const auto &assets = model.editor.assets;
    if (!assets)
        return nullptr;
    const auto it = std::find_if(assets->begin(), assets->end(), [&](const auto &a) { return a.key == key; });
    return it == assets->end() ? nullptr : &*it;
}
std::string asset_title(const Model &model, const std::string &key) {
    const auto *asset = find_asset(model, key);
    return asset && !asset->title.empty() ? asset->title : key;
}
bool editing_blocked(const ParkEditorUI &ui, const Model &model) {
    return model.editor.busy || model.editor.failed || ui.pending || ui.preview_token || !model.editor.available;
}
// One card of the library: the item's picture over its title, white while chosen. `drag` makes it a
// drag source for the viewport. Returns whether it was clicked.
bool asset_card(ParkEditorUI &ui, const std::string &key, const std::string &title, const char *detail,
                ImVec2 size, unsigned seed, bool chosen, bool drag, unsigned title_lines) {
    const float k = ui.ui_scale;
    const auto top = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered();
    auto *draw = ImGui::GetWindowDrawList();
    theme::tile_rect(draw, top, plus(top, size),
                            chosen    ? theme::focus_fill
                            : hovered ? skate_theme::tile_light
                                      : skate_theme::tile_grey,
                            seed, k);
    const float inset = 6 * k, picture = std::max(8.0f, size.x - inset * 2);
    ParkPreviewImage image;
    const bool has_preview = park_preview_image(key, image);
    if (has_preview)
        draw->AddImage(image.texture, plus(top, {inset, inset}), plus(top, {inset + picture, inset + picture}),
                       image.uv0, image.uv1);
    else {
        draw->AddRectFilled(plus(top, {inset, inset}), plus(top, {inset + picture, inset + picture}),
                            chosen ? IM_COL32(220, 222, 226, 255) : IM_COL32(37, 37, 39, 255));
        const char *label = "No preview";
        if (const auto extent = ImGui::CalcTextSize(label); extent.x <= picture - 4 * k)
            draw->AddText(plus(top, {(size.x - extent.x) * .5f, inset + picture * .5f - extent.y * .5f}), muted, label);
    }
    const ImVec2 text_at = plus(top, {inset + 1, inset + picture + 4 * k});
    draw->PushClipRect(text_at, plus(top, {size.x - inset, size.y - 3 * k}), true);
    const auto wrapped = card_title(title, size.x - inset * 2 - 2, title_lines);
    draw->AddText(text_at, chosen ? skate_theme::black : paper, wrapped.c_str());
    draw->PopClipRect();
    if (drag && ImGui::BeginDragDropSource()) {
        ui.asset_drag = true;
        ImGui::SetDragDropPayload("PARK_ASSET", key.c_str(), key.size() + 1);
        ImGui::TextUnformatted(title.c_str());
        if (has_preview)
            ImGui::Image(image.texture, {112 * k, 112 * k}, image.uv0, image.uv1);
        ImGui::EndDragDropSource();
    }
    if (hovered && !ImGui::GetDragDropPayload()) {
        ImGui::BeginTooltip();
        if (has_preview)
            ImGui::Image(image.texture, {192 * k, 192 * k}, image.uv0, image.uv1);
        ImGui::TextUnformatted(title.c_str());
        if (detail && *detail)
            ImGui::TextDisabled("%s", detail);
        ImGui::TextDisabled("%s", key.c_str());
        ImGui::EndTooltip();
    }
    return clicked;
}
// The objects the library's view lists: a category's (or all), or the ones picked lately.
std::vector<const EditorAsset *> listed_assets(const ParkEditorUI &ui, const Model &model) {
    std::vector<const EditorAsset *> listed;
    const auto &assets = model.editor.assets;
    if (!assets)
        return listed;
    if (ui.library_view == ParkLibraryView::recent) {
        for (const auto &key : ui.recent)
            if (const auto *asset = find_asset(model, key); asset && matches(asset->title + " " + asset->key, ui.search.data()))
                listed.push_back(asset);
        return listed;
    }
    for (const auto &item : *assets)
        if ((ui.category.empty() || ui.category == item.category) &&
            matches(item.title + " " + item.key, ui.search.data()))
            listed.push_back(&item);
    return listed;
}
// A strip the wheel scrolls sideways (a mouse has no sideways wheel), `pitch` a step.
void wheel_scrolls_sideways(float pitch) {
    const auto &io = ImGui::GetIO();
    if (io.MouseWheel != 0 && ImGui::IsWindowHovered())
        ImGui::SetScrollX(std::clamp(ImGui::GetScrollX() - io.MouseWheel * pitch, 0.0f, ImGui::GetScrollMaxX()));
}
// The cards from `count` that the strip shows at its scroll, given their pitch.
std::pair<int, int> visible_cards(int count, float pitch) {
    const float from = ImGui::GetScrollX(), width = ImGui::GetWindowWidth();
    const int first = std::clamp(static_cast<int>(from / pitch), 0, count);
    const int last = std::clamp(static_cast<int>((from + width) / pitch) + 2, first, count);
    return {first, last};
}
std::size_t missing_objects(const Model &model, const EditorObjectSet &set) {
    std::size_t missing = 0;
    if (model.editor.assets)
        for (const auto &object : set.objects)
            missing += find_asset(model, object.item) == nullptr;
    return missing;
}
std::vector<const EditorObjectSet *> listed_sets(const ParkEditorUI &ui, const Model &model) {
    std::vector<const EditorObjectSet *> listed;
    if (const auto &sets = model.editor.object_sets)
        for (const auto &set : *sets)
            if (matches(set.name + " " + set.map, ui.search.data()))
                listed.push_back(&set);
    return listed;
}
// Name the selection and save it as a set: the form above the sets.
void set_form(ParkEditorUI &ui, const Model &model) {
    const float k = ui.ui_scale;
    const auto &state = model.editor;
    if (ui.focus_set_name) {
        ImGui::SetKeyboardFocusHere();
        ui.focus_set_name = false;
    }
    ImGui::SetNextItemWidth(-1);
    const bool enter = ImGui::InputTextWithHint("##set-name", "Set name", ui.set_name.data(), ui.set_name.size(),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    const auto count = ui.selection.size();
    const bool can_save = !editing_blocked(ui, model) && count && ui.set_name[0];
    const auto label = count ? "SAVE " + std::to_string(count) + (count == 1 ? " OBJECT" : " OBJECTS") + " AS SET"
                             : std::string("SELECT OBJECTS TO SAVE");
    if ((chip(ui, (label + "##save-set").c_str(), Chip::blue, 34 * k, 77, can_save, ImGui::GetContentRegionAvail().x) ||
         enter) &&
        can_save)
        save_set(ui, state);
    const std::string typed = ui.set_name.data();
    if (state.object_sets && !typed.empty() &&
        std::any_of(state.object_sets->begin(), state.object_sets->end(), [&](const auto &set) { return set.name == typed; }))
        ImGui::TextDisabled("Saving replaces the set with this name.");
}
// Place / Delete under a set; Delete asks again before it deletes.
void set_buttons(ParkEditorUI &ui, const Model &model, const EditorObjectSet &set, float width, bool missing) {
    const bool busy = editing_blocked(ui, model);
    const bool chosen = ui.placing_set == set.name;
    const float buttons = (width - ImGui::GetStyle().ItemSpacing.x) * .5f;
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
            delete_set(ui, model.editor, set.name);
        else
            ui.set_delete = set.name;
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
}
void set_summary(const Model &model, const EditorObjectSet &set) {
    if (const auto missing = missing_objects(model, set))
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(skate_theme::danger), "%zu objects  /  %zu unavailable",
                           set.objects.size(), missing);
    else
        ImGui::TextDisabled("%zu objects  /  saved on %s", set.objects.size(), set.map.c_str());
}
std::vector<std::size_t> listed_scene(const ParkEditorUI &ui, const Model &model, bool search) {
    std::vector<std::size_t> rows;
    for (std::size_t i = 0; i < model.editor.objects.size(); ++i)
        if (!search || !ui.search[0] || matches(asset_title(model, model.editor.objects[i].item), ui.search.data()))
            rows.push_back(i);
    return rows;
}
bool in_selection(const ParkEditorUI &ui, std::uint64_t id) {
    return std::find(ui.selection.begin(), ui.selection.end(), id) != ui.selection.end();
}
} // namespace

float chip_width(const ParkEditorUI &ui, const char *label, float text_size) {
    const float k = ui.ui_scale;
    return font_or(ui.bold_font)->CalcTextSizeA(text_size * k, FLT_MAX, 0, label, ImGui::FindRenderedTextEnd(label)).x +
           24 * k;
}
bool chip(const ParkEditorUI &ui, const char *label, Chip style, float height, unsigned seed, bool enabled, float width,
          float text_size) {
    const float k = ui.ui_scale;
    ImFont *font = font_or(ui.bold_font);
    const char *end = ImGui::FindRenderedTextEnd(label);
    const auto extent = font->CalcTextSizeA(text_size * k, FLT_MAX, 0, label, end);
    const ImVec2 size{width > 0 ? width : extent.x + 24 * k, height};
    const auto at = ImGui::GetCursorScreenPos();
    // Inside a BeginDisabled block too: the chip greys like the buttons around it.
    enabled = enabled && !(GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::InvisibleButton(label, size);
    ImGui::EndDisabled();
    const bool hovered = enabled && ImGui::IsItemHovered();
    ImU32 fill = 0, ink = paper;
    switch (style) {
    case Chip::plain:
        fill = hovered ? skate_theme::tile_grey : 0;
        ink = hovered ? paper : IM_COL32(185, 187, 192, 255);
        break;
    case Chip::tile:
        fill = hovered ? skate_theme::tile_light : skate_theme::tile_grey;
        break;
    case Chip::blue:
        fill = hovered ? skate_theme::blue_hover : blue;
        ink = skate_theme::black;
        break;
    case Chip::white:
        // (The light grey of skate.'s chosen tab: the blue is the focus's and the active tool's.)
        fill = hovered ? IM_COL32(246, 246, 248, 255) : IM_COL32(226, 227, 230, 255);
        ink = skate_theme::black;
        break;
    case Chip::danger:
        fill = hovered ? skate_theme::tile_light : skate_theme::tile_grey;
        ink = IM_COL32(255, 138, 138, 255);
        break;
    }
    if (!enabled) {
        // Dimmed the way ImGui dims a disabled widget, so a greyed chip reads like a greyed button.
        fill = fill ? (fill & 0x00ffffffu) | (static_cast<ImU32>(((fill >> 24) & 255) * .45f) << 24) : 0;
        ink = style == Chip::blue || style == Chip::white ? IM_COL32(20, 20, 20, 200) : IM_COL32(93, 95, 100, 255);
    }
    auto *draw = ImGui::GetWindowDrawList();
    if (fill)
        theme::tile_rect(draw, at, plus(at, size), fill, seed, k);
    draw->AddText(font, text_size * k, {at.x + (size.x - extent.x) * .5f, at.y + (size.y - extent.y) * .5f}, ink, label,
                  end);
    return pressed;
}
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

std::vector<LibraryChip> library_chips(const Model &model, bool views) {
    std::set<std::string> categories;
    if (const auto &assets = model.editor.assets)
        for (const auto &item : *assets)
            categories.insert(item.category);
    std::vector<LibraryChip> chips{{"ALL", {}, ParkLibraryView::objects}};
    for (const auto &category : categories)
        chips.push_back({upper(category) + "##" + category, category, ParkLibraryView::objects});
    chips.push_back({"RECENT", {}, ParkLibraryView::recent});
    if (views) {
        chips.push_back({"SETS", {}, ParkLibraryView::sets});
        chips.push_back({"SCENE", {}, ParkLibraryView::scene});
    }
    return chips;
}
namespace {
bool chip_on(const ParkEditorUI &ui, const LibraryChip &entry) {
    return ui.library_view == entry.view && (entry.view != ParkLibraryView::objects || ui.category == entry.category);
}
void choose_chip(ParkEditorUI &ui, const LibraryChip &entry) {
    if (entry.view == ParkLibraryView::objects)
        ui.category = entry.category;
    show_library(ui, entry.view);
}
} // namespace
void step_library_chip(ParkEditorUI &ui, const Model &model, int step, bool views) {
    const auto chips = library_chips(model, views);
    const auto lit = std::find_if(chips.begin(), chips.end(), [&](const auto &entry) { return chip_on(ui, entry); });
    const int count = static_cast<int>(chips.size());
    const int from = lit == chips.end() ? 0 : static_cast<int>(lit - chips.begin());
    choose_chip(ui, chips[static_cast<std::size_t>(((from + step) % count + count) % count)]);
    ui.chip_scroll = true;
}
void library_chip_row(ParkEditorUI &ui, const Model &model, bool views, bool wrap, float height, float width) {
    const float k = ui.ui_scale, gap = 4 * k;
    const auto chips = library_chips(model, views);
    if (wrap) {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        for (std::size_t i = 0; i < chips.size(); ++i) {
            const float chip_size = chip_width(ui, chips[i].label.c_str(), 13);
            if (i && ImGui::GetItemRectMax().x + gap + chip_size <= right)
                ImGui::SameLine(0, gap);
            const bool on = chip_on(ui, chips[i]);
            if (chip(ui, chips[i].label.c_str(), on ? Chip::white : Chip::tile, height, static_cast<unsigned>(130 + i), true,
                     0, 13))
                choose_chip(ui, chips[i]);
        }
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
    ImGui::BeginChild("chip-row", {width > 0 ? width : ImGui::GetContentRegionAvail().x, height}, 0,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    wheel_scrolls_sideways(80 * k);
    for (std::size_t i = 0; i < chips.size(); ++i) {
        if (i)
            ImGui::SameLine(0, gap);
        const bool on = chip_on(ui, chips[i]);
        if (chip(ui, chips[i].label.c_str(), on ? Chip::white : Chip::plain, height, static_cast<unsigned>(130 + i)))
            choose_chip(ui, chips[i]);
        if (on && ui.chip_scroll) {
            ImGui::SetScrollHereX(.5f);
            ui.chip_scroll = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}
void library_search(ParkEditorUI &ui, const Model &model, float width) {
    std::string hint = "Search objects...";
    if (ui.library_view == ParkLibraryView::sets)
        hint = "Search sets...";
    else if (ui.library_view == ParkLibraryView::scene)
        hint = "Search the scene...";
    else if (ui.library_view == ParkLibraryView::recent)
        hint = "Search recent objects...";
    else if (model.editor.assets)
        hint = "Search " + std::to_string(model.editor.assets->size()) + " objects...";
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint("##library-search", hint.c_str(), ui.search.data(), ui.search.size());
}

void object_grid(ParkEditorUI &ui, const Model &model, int columns) {
    const float k = ui.ui_scale;
    const auto listed = listed_assets(ui, model);
    ImGui::TextDisabled("%zu objects  /  click or drag to place", listed.size());
    if (listed.empty() && ui.library_view == ParkLibraryView::recent)
        ImGui::TextDisabled("Objects you pick to place show here.");
    ImGui::BeginChild("library-grid", {0, 0});
    if (columns <= 0)
        columns = std::max(2, static_cast<int>(ImGui::GetContentRegionAvail().x / (120 * k)));
    const float card_width = ImGui::GetContentRegionAvail().x / static_cast<float>(columns) - 8;
    const float card_height = card_width + ImGui::GetTextLineHeight() * 3 + 12 * k;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {4, 4});
    if (ImGui::BeginTable("asset-cards", columns, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_NoSavedSettings)) {
        ImGuiListClipper clipper;
        clipper.Begin((static_cast<int>(listed.size()) + columns - 1) / columns, card_height + 8);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                ImGui::TableNextRow(0, card_height + 8);
                for (int column = 0; column < columns; ++column) {
                    const int index = row * columns + column;
                    if (index >= static_cast<int>(listed.size()))
                        break;
                    ImGui::TableSetColumnIndex(column);
                    const auto &item = *listed[static_cast<std::size_t>(index)];
                    ImGui::PushID(item.key.c_str());
                    if (asset_card(ui, item.key, item.title, item.category.c_str(), {card_width, card_height},
                                   static_cast<unsigned>(index + 7), ui.placing == item.key, true, 3))
                        begin_placement(ui, model, item.key);
                    ImGui::PopID();
                }
            }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
}
void object_strip(ParkEditorUI &ui, const Model &model, float card_width) {
    const float k = ui.ui_scale, gap = 10 * k, pitch = card_width + gap;
    const auto listed = listed_assets(ui, model);
    ImGui::BeginChild("library-strip", {0, 0}, 0,
                      ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    wheel_scrolls_sideways(pitch * 2);
    const float height = ImGui::GetContentRegionAvail().y;
    if (listed.empty())
        ImGui::TextDisabled(ui.library_view == ParkLibraryView::recent ? "Objects you pick to place show here."
                            : ui.search[0]                              ? "No object matches the search."
                                                                        : "No objects in the library yet.");
    const int count = static_cast<int>(listed.size());
    const auto [first, last] = visible_cards(count, pitch);
    const auto start = ImGui::GetCursorScreenPos();
    for (int index = first; index < last; ++index) {
        const auto &item = *listed[static_cast<std::size_t>(index)];
        ImGui::SetCursorScreenPos({start.x + static_cast<float>(index) * pitch, start.y});
        ImGui::PushID(item.key.c_str());
        if (asset_card(ui, item.key, item.title, item.category.c_str(), {card_width, height},
                       static_cast<unsigned>(index + 7), ui.placing == item.key, true, 2))
            begin_placement(ui, model, item.key);
        ImGui::PopID();
    }
    // The strip's full width, so it scrolls to the last card.
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({std::max(1.0f, static_cast<float>(count) * pitch - gap), 1});
    ImGui::EndChild();
}

void set_list(ParkEditorUI &ui, const Model &model) {
    const float k = ui.ui_scale;
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Select objects (Shift-click adds), name them and save them as a set. "
                        "Place it again anywhere, on any map.");
    ImGui::PopTextWrapPos();
    set_form(ui, model);
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##set-search", "Search sets...", ui.search.data(), ui.search.size());
    const auto listed = listed_sets(ui, model);
    ImGui::TextDisabled("%zu sets  /  Place, then click in the world", listed.size());
    ImGui::Spacing();
    ImGui::BeginChild("set-list", {0, 0});
    if (listed.empty())
        ImGui::TextDisabled(model.editor.object_sets && !model.editor.object_sets->empty() ? "No set matches the search."
                                                                                          : "No saved sets yet.");
    for (std::size_t index = 0; index < listed.size(); ++index) {
        const auto &set = *listed[index];
        ImGui::PushID(set.name.c_str());
        auto *draw = ImGui::GetWindowDrawList();
        const auto top = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetTextLineHeight() * 2 + ImGui::GetFrameHeight() + 26 * k;
        const bool chosen = ui.placing_set == set.name;
        theme::tile_rect(draw, top, plus(top, {width, height}),
                                chosen ? skate_theme::tile_light : skate_theme::tile_grey,
                                static_cast<unsigned>(index + 91), k);
        if (chosen)
            draw->AddRect(plus(top, {-1, -1}), plus(top, {width + 1, height + 1}), blue, 0, 0, 3 * k);
        ImGui::SetCursorScreenPos(plus(top, {10 * k, 8 * k}));
        ImGui::BeginGroup();
        ImGui::PushFont(font_or(ui.bold_font));
        ImGui::TextUnformatted(set.name.c_str());
        ImGui::PopFont();
        set_summary(model, set);
        set_buttons(ui, model, set, width - 20 * k, missing_objects(model, set) != 0);
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(plus(top, {0, height + 6 * k}));
        ImGui::Dummy({width, 0});
        ImGui::PopID();
    }
    ImGui::EndChild();
}
void set_strip(ParkEditorUI &ui, const Model &model, float card_width) {
    const float k = ui.ui_scale, gap = 10 * k;
    // The form on the left, the sets beside it.
    const float form = std::min(260 * k, ImGui::GetContentRegionAvail().x * .35f);
    ImGui::BeginChild("set-form", {form, 0});
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("Select objects (Shift-click adds), name them and save them as a set to place anywhere.");
    ImGui::PopTextWrapPos();
    set_form(ui, model);
    ImGui::EndChild();
    ImGui::SameLine(0, gap);
    const auto listed = listed_sets(ui, model);
    const float width = card_width * 1.7f, pitch = width + gap;
    ImGui::BeginChild("set-strip", {0, 0}, 0, ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    wheel_scrolls_sideways(pitch);
    const float height = ImGui::GetContentRegionAvail().y;
    if (listed.empty())
        ImGui::TextDisabled(model.editor.object_sets && !model.editor.object_sets->empty() ? "No set matches the search."
                                                                                          : "No saved sets yet.");
    const int count = static_cast<int>(listed.size());
    const auto [first, last] = visible_cards(count, pitch);
    const auto start = ImGui::GetCursorScreenPos();
    auto *draw = ImGui::GetWindowDrawList();
    for (int index = first; index < last; ++index) {
        const auto &set = *listed[static_cast<std::size_t>(index)];
        const ImVec2 top{start.x + static_cast<float>(index) * pitch, start.y};
        ImGui::PushID(set.name.c_str());
        const bool chosen = ui.placing_set == set.name;
        theme::tile_rect(draw, top, plus(top, {width, height}),
                                chosen ? skate_theme::tile_light : skate_theme::tile_grey,
                                static_cast<unsigned>(index + 91), k);
        if (chosen)
            draw->AddRect(plus(top, {-1, -1}), plus(top, {width + 1, height + 1}), blue, 0, 0, 3 * k);
        // Up to three of its objects' pictures across the top, to tell the sets apart.
        const float inset = 8 * k;
        const float picture = std::min((width - inset * 4) / 3,
                                       height - ImGui::GetTextLineHeightWithSpacing() * 2 - ImGui::GetFrameHeight() - inset * 3);
        for (std::size_t i = 0; i < std::min<std::size_t>(3, set.objects.size()) && picture > 8; ++i) {
            const ImVec2 at = plus(top, {inset + static_cast<float>(i) * (picture + inset), inset});
            ParkPreviewImage image;
            if (park_preview_image(set.objects[i].item, image))
                draw->AddImage(image.texture, at, plus(at, {picture, picture}), image.uv0, image.uv1);
            else
                draw->AddRectFilled(at, plus(at, {picture, picture}), IM_COL32(37, 37, 39, 255));
        }
        ImGui::SetCursorScreenPos(plus(top, {inset, inset * 2 + std::max(0.0f, picture)}));
        ImGui::BeginGroup();
        ImGui::PushFont(font_or(ui.bold_font));
        ImGui::TextUnformatted(set.name.c_str());
        ImGui::PopFont();
        set_summary(model, set);
        set_buttons(ui, model, set, width - inset * 2, missing_objects(model, set) != 0);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({std::max(1.0f, static_cast<float>(count) * pitch - gap), 1});
    ImGui::EndChild();
}

void scene_list(ParkEditorUI &ui, const Model &model, bool search) {
    const auto rows = listed_scene(ui, model, search);
    ImGui::BeginChild("scene-list", {0, 0});
    if (rows.empty())
        ImGui::TextDisabled(model.editor.objects.empty() ? "Nothing placed yet." : "No object matches the search.");
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(rows.size()));
    while (clipper.Step())
        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
            const auto &row = model.editor.objects[rows[static_cast<std::size_t>(i)]];
            const auto label = asset_title(model, row.item) + "##" + std::to_string(row.id);
            ImGui::BeginDisabled(ui.preview_token || ui.pending || model.editor.busy);
            if (ImGui::Selectable(label.c_str(), in_selection(ui, row.id)))
                choose(ui, model.editor, row, ImGui::GetIO().KeyShift);
            ImGui::EndDisabled();
        }
    ImGui::EndChild();
}
void scene_strip(ParkEditorUI &ui, const Model &model, float card_width) {
    const float k = ui.ui_scale, gap = 10 * k, pitch = card_width + gap;
    const auto rows = listed_scene(ui, model, true);
    ImGui::BeginChild("scene-strip", {0, 0}, 0,
                      ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    wheel_scrolls_sideways(pitch * 2);
    const float height = ImGui::GetContentRegionAvail().y;
    if (rows.empty())
        ImGui::TextDisabled(model.editor.objects.empty() ? "Nothing placed yet. Pick an object and click in the world."
                                                         : "No object matches the search.");
    const int count = static_cast<int>(rows.size());
    const auto [first, last] = visible_cards(count, pitch);
    const auto start = ImGui::GetCursorScreenPos();
    const bool locked = ui.preview_token || ui.pending || model.editor.busy;
    for (int index = first; index < last; ++index) {
        const auto &row = model.editor.objects[rows[static_cast<std::size_t>(index)]];
        ImGui::SetCursorScreenPos({start.x + static_cast<float>(index) * pitch, start.y});
        ImGui::PushID(static_cast<int>(row.id));
        const auto detail = "Object " + std::to_string(row.id);
        ImGui::BeginDisabled(locked);
        if (asset_card(ui, row.item, asset_title(model, row.item), detail.c_str(), {card_width, height},
                       static_cast<unsigned>(index + 211), in_selection(ui, row.id), false, 2))
            choose(ui, model.editor, row, ImGui::GetIO().KeyShift);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy({std::max(1.0f, static_cast<float>(count) * pitch - gap), 1});
    ImGui::EndChild();
}

std::string selection_title(const ParkEditorUI &ui, const Model &model) {
    if (!ui.placing_set.empty())
        return "PLACING A SET";
    if (!ui.placing.empty())
        return "PLACING";
    if (ui.selection.size() > 1)
        return "SELECTED " + std::string(middot) + " " + std::to_string(ui.selection.size());
    if (const auto *object = selected_object(ui, model.editor))
        return upper(asset_title(model, object->item));
    return "NOTHING SELECTED";
}
std::string selection_detail(const ParkEditorUI &ui, const Model &model) {
    if (!ui.placing_set.empty())
        return ui.placing_set;
    if (!ui.placing.empty())
        return asset_title(model, ui.placing);
    if (ui.selection.size() > 1) {
        std::string names;
        std::size_t shown = 0;
        for (const auto &object : model.editor.objects)
            if (in_selection(ui, object.id) && shown++ < 3)
                names += (names.empty() ? "" : ", ") + asset_title(model, object.item);
        if (shown > 3)
            names += ", ...";
        return names + "  " + middot + "  shared pivot";
    }
    if (const auto *object = selected_object(ui, model.editor))
        return "Object " + std::to_string(object->id);
    return "Click an object, or pick one from the library.";
}
void transform_fields(ParkEditorUI &ui, const Model &model, const CallbacksV3 &callbacks) {
    const float k = ui.ui_scale;
    const auto *selected = selected_object(ui, model.editor);
    const bool editable = selected || !ui.placing.empty() || !ui.placing_set.empty();
    const bool own_placement = ui.preview_token && ui.preview_place && model.editor.preview_token == ui.preview_token;
    ImGui::BeginDisabled(!editable || ui.pending || (model.editor.busy && !own_placement) ||
                         (ui.preview_token && !own_placement));
    // Label column, as wide as its longest label in this font, then the values filling the row.
    const float label = ImGui::GetCursorPosX() +
                        std::max(ImGui::CalcTextSize("Position").x, ImGui::CalcTextSize("Rotation").x) + 10 * k;
    const auto row = [&](const char *text) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text);
        ImGui::SameLine(label);
        ImGui::SetNextItemWidth(-1);
    };
    row("Position");
    if (ImGui::DragFloat3("##position", ui.position.data(), .1f, -100000, 100000, "%.3f"))
        ui.inspector_dirty = true;
    row("Rotation");
    const auto rotation_format = std::string("%.1f") + degrees;
    if (ImGui::DragFloat3("##rotation", ui.angles.data(), 1, -36000, 36000, rotation_format.c_str()))
        ui.inspector_dirty = true;
    row(ui.selection.size() > 1 ? "Scale x" : "Scale");
    ImGui::BeginDisabled(!ui.scaling_allowed);
    if (ImGui::DragFloat("##scale", &ui.scale, .01f, .01f, 100, "%.3f", ImGuiSliderFlags_AlwaysClamp))
        ui.inspector_dirty = true;
    ImGui::EndDisabled();
    if (!ui.scaling_allowed)
        ImGui::TextDisabled("This server has object scaling off.");
    ImGui::Dummy({0, 4 * k});
    const float height = 34 * k, spacing = ImGui::GetStyle().ItemSpacing.x;
    const float full = ImGui::GetContentRegionAvail().x, half = (full - spacing) * .5f;
    ImGui::BeginDisabled(!selected && ui.surface_mode && !ui.surface_valid && !ui.inspector_dirty);
    const bool apply = chip(ui,
                            selected                   ? "APPLY##apply"
                            : !ui.placing_set.empty() ? "PLACE SET AT COORDINATES##apply"
                                                      : "PLACE AT COORDINATES##apply",
                            Chip::blue, height, 71, true, selected ? half : full);
    if (ImGui::IsItemHovered() && selected)
        ImGui::SetTooltip("Move the selection to the values above (snapped while Snap is on).");
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
        ImGui::SameLine(0, spacing);
        if (chip(ui, "SAVE AS SET##save-as-set", Chip::tile, height, 72, !ui.selection.empty(), half)) {
            // With a name typed, save at once; else the sets, with the name field ready for one.
            if (ui.set_name[0])
                save_set(ui, model.editor);
            else {
                show_library(ui, ParkLibraryView::sets);
                ui.focus_set_name = true;
                ui.feedback = "Name the set, then save it.";
                ui.feedback_until = ImGui::GetTime() + 5;
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save the selected objects as a set, to place again anywhere.");
        if (chip(ui, (group ? "DUPLICATE ALL##duplicate" : "DUPLICATE##duplicate"), Chip::tile, height, 73, true, half))
            duplicate_selection(ui, model.editor);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy %s with %s rotation and scale, one grid step along X (Ctrl+D).",
                              group ? "every selected object" : "the object", group ? "their" : "its");
        ImGui::SameLine(0, spacing);
        const auto label_delete =
            (group ? "DELETE " + std::to_string(ui.selection.size()) : std::string("DELETE")) + "##delete";
        if (chip(ui, label_delete.c_str(), Chip::danger, height, 74, true, half)) {
            if (group)
                delete_selection(ui, model.editor);
            else
                send(ui, model, callbacks, "delete");
        }
    }
    ImGui::EndDisabled();
}
std::string snap_label(const ParkEditorUI &ui) {
    if (!ui.snapping)
        return "SNAP OFF";
    // 0.50 reads as 0.5, 2.00 as 2: the grid as the player would say it.
    auto grid = std::format("{:.2f}", ui.grid);
    while (grid.back() == '0')
        grid.pop_back();
    if (grid.back() == '.')
        grid.pop_back();
    return std::format("SNAP {} m {} {:.0f}{}", grid, middot, ui.angle_snap, degrees);
}
void placement_settings(ParkEditorUI &ui) {
    const float k = ui.ui_scale;
    if (ImGui::Checkbox("Place on surfaces", &ui.surface_mode)) {
        ui.surface_confirm = false;
        ui.surface_valid = false;
    }
    ImGui::Checkbox("Snap to grid / angles", &ui.snapping);
    ImGui::Checkbox("Local axes", &ui.local_axes);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(ui.selection.size() > 1
                              ? "Move and rotate along the group's own axes: a placed set's, or the last selected object's."
                              : "Move and rotate along the object's own axes.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(90 * k);
    ImGui::DragFloat("Grid (m)", &ui.grid, .05f, .05f, 100, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(90 * k);
    ImGui::DragFloat("Angle step", &ui.angle_snap, 1, 1, 90, "%.0f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SetNextItemWidth(90 * k);
    ImGui::DragFloat("Scale step", &ui.scale_snap, .01f, .01f, 10, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    if (ui.surface_mode) {
        ImGui::SetNextItemWidth(90 * k);
        ImGui::DragFloat("Lift (m)", &ui.surface_offset, .05f, -100, 100, "%.2f", ImGuiSliderFlags_AlwaysClamp);
    } else {
        ImGui::SetNextItemWidth(90 * k);
        ImGui::DragFloat("Build plane Y", &ui.plane_height, .1f, -10000, 10000, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SetNextItemWidth(90 * k);
        ImGui::DragFloat("Ray distance", &ui.distance, .5f, 1, 500, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    }
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled(ui.surface_mode ? "Placement follows game collision." : "Placement uses the grid plane.");
    ImGui::TextDisabled("Wire cube marks the object origin.");
    ImGui::PopTextWrapPos();
}
} // namespace dingosdk::overlay::park_editor_detail
