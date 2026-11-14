#include "gui_internal.h"
#include "gui_renderer.h"

#include "Engine/Vfs/mod_catalog.h"
#include "Engine/Vfs/mod_items.h"

#include "Engine/Core/Log/logging.h"

#include <algorithm>
#include <cctype>
#include <format>

// A mod overview's ITEMS tab: the items the mod adds, as tiles with the thumbnails the game's own item
// grid shows, each ticked to load or not. A pack of 200 decks of which a player wants a few: the ones
// left out, and what only they use, are not merged into the game at all (mods::items_left_out,
// Engine/Vfs/mod_catalog.h). The items are read from the mod's own files (Engine/Vfs/mod_items.h) on a
// worker, so a pack lists them as soon as it is installed; a choice is kept as it is made and is in
// the game the next time Skate starts.
namespace dingosdk::launcher_gui::detail {
namespace {

constexpr std::uint32_t thumbnail_side = 96;   // pixels; the tiles draw them at up to this size
constexpr int atlas_columns = 10, atlas_cells = atlas_columns * atlas_columns;
constexpr std::size_t most_atlases = 8;        // 800 thumbnails; an item past that shows its name only

std::string folded_text(std::string_view text) {
    std::string out(text);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// An item's name as the game's grid gives a mod's item: its key without "own_", words apart.
std::string item_title(std::string_view key) {
    std::string title(key.starts_with("own_") ? key.substr(4) : key);
    std::replace(title.begin(), title.end(), '_', ' ');
    return title;
}

// What kind of item an item's folder says it is, for a player: "cust_tops" is Tops, "board_wheelcolor"
// Wheels. One this does not know is its own name made readable.
std::string category_label(std::string_view category) {
    struct Known { const char* word; const char* label; };
    static constexpr Known known[] = {
        {"fullbodycostume", "Costumes"}, {"costume", "Costumes"}, {"outfit", "Outfits"}, {"top", "Tops"}, {"bottom", "Bottoms"},
        {"shoe", "Shoes"}, {"sock", "Socks"}, {"hat", "Headwear"}, {"headwear", "Headwear"}, {"eyewear", "Eyewear"},
        {"earwear", "Earwear"}, {"handwear", "Gloves"}, {"face_accessory", "Face"}, {"hair", "Hair"}, {"beard", "Facial hair"},
        {"tattoo", "Tattoos"}, {"sticker", "Stickers"}, {"grip", "Grip tape"}, {"wheel", "Wheels"}, {"truck", "Trucks"},
        {"deck", "Decks"}, {"gesture", "Gestures"}, {"emote", "Gestures"}, {"playercard", "Player cards"}, {"icon", "Player icons"},
        {"background", "Player cards"}, {"bkobject", "Objects"}};
    const auto text = folded_text(category);
    for (const auto& entry : known)
        if (text.find(entry.word) != std::string::npos) return entry.label;
    std::string label(text);
    for (const auto prefix : {"cust_", "board_", "type_"})
        if (label.starts_with(prefix)) { label.erase(0, std::string_view(prefix).size()); break; }
    while (!label.empty() && std::isdigit(static_cast<unsigned char>(label.back()))) label.pop_back();
    std::replace(label.begin(), label.end(), '_', ' ');
    if (label.empty()) return "Other";
    label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    return label;
}

// The atlases are let go of, not freed: the frame being drawn may already have tiles of them in its
// draw list (the overview closes in the middle of its own frame), and a texture freed under a draw
// call that samples it takes the graphics driver down (2026-11-14). free_retired frees them once
// that frame has been drawn.
void release_atlases(ModItemsView& view) {
    for (const auto id : view.atlases)
        if (id) view.retired.emplace_back(id, ImGui::GetFrameCount());
    view.atlases.clear();
}
void free_retired(ModItemsView& view) {
    std::erase_if(view.retired, [](const std::pair<ImTextureID, int>& entry) {
        if (entry.second >= ImGui::GetFrameCount() - 1) return false;
        if (g_renderer) g_renderer->release_texture(entry.first);
        return true;
    });
}

// Where a mod's items are: its own folder, or each of its variants' (a mod with variants ships its
// data in them, and a choice is the mod's whichever is in play).
std::vector<fs::path> item_folders(const mods::Mod& mod) {
    std::vector<fs::path> folders;
    if (mod.variants)
        for (const auto& variant : mods::installed_variants(mod))
            if (auto folder = mods::variant_folder(mod, variant); !folder.empty()) folders.push_back(std::move(folder));
    if (folders.empty() || mod.provides_layout) folders.insert(folders.begin(), mod.directory);
    return folders;
}

void start_read(ModsPanel& panel) {
    auto& view = panel.items;
    if (view.busy || view.wanted.empty()) return;
    if (view.worker.joinable()) view.worker.join();
    view.busy = true;
    const auto name = view.wanted;
    const auto folders = view.wanted_folders;
    const auto data_root = panel.root.parent_path();
    view.wanted.clear();
    view.worker = std::thread([&view, name, folders, data_root] {
        mods::ModItems all;
        std::set<std::string> seen;
        for (const auto& folder : folders) {
            auto read = mods::read_mod_items(data_root, folder, thumbnail_side);
            if (!read.problem.empty() && all.problem.empty()) all.problem = std::move(read.problem);
            for (auto& item : read.items)
                if (seen.insert(item.key).second) all.items.push_back(std::move(item));
        }
        {
            std::lock_guard lock(view.mutex);
            view.result.ready = true;
            view.result.mod = name;
            view.result.items = std::move(all);
        }
        view.busy = false;
    });
}

void save(ModsPanel& panel, const mods::Mod& mod) {
    auto& view = panel.items;
    if (!mods::save_items_left_out(mod.name, view.off)) {
        view.error = "The choice could not be saved (%LOCALAPPDATA%\\ReSkate\\items-left-out.json).";
        return;
    }
    view.error.clear();
    const auto name = folded_text(mod.name);
    if (view.off.empty()) panel.items_off.erase(name);
    else panel.items_off[name] = view.off.size();
}

// The items of the mod's that are left out: the choice may hold keys of an older version of it.
std::size_t left_out(const ModItemsView& view) {
    return static_cast<std::size_t>(std::count_if(view.list.begin(), view.list.end(),
        [&](const ModItemsView::Item& item) { return view.off.contains(item.key); }));
}

} // namespace

bool mod_may_have_items(const mods::Mod& mod) {
    // (A mod that adds levels is merged whole: mod_merge_overrides.cpp.)
    return (mod.provides_layout || mod.variants) && !mod.provides_levels;
}

void open_mod_items(ModsPanel& panel, const mods::Mod& mod) {
    auto& view = panel.items;
    release_atlases(view);
    view.mod = mod.name;
    view.tab = false;
    view.read = false;
    view.list.clear();
    view.categories.clear();
    view.category.clear();
    view.search.fill(0);
    view.problem.clear();
    view.error.clear();
    view.off = mods::items_left_out(mod.name);
    view.wanted.clear();
    if (!mod_may_have_items(mod)) {
        view.read = true;
        return;
    }
    view.wanted = mod.name;
    view.wanted_folders = item_folders(mod);
    start_read(panel);
}

void close_mod_items(ModsPanel& panel) {
    auto& view = panel.items;
    release_atlases(view);
    view.mod.clear();
    view.tab = false;
    view.list.clear();
    view.categories.clear();
    view.wanted.clear();
}

void read_items_left_out(ModsPanel& panel) {
    panel.items_off.clear();
    for (const auto& [mod, keys] : mods::items_left_out()) panel.items_off[mod] = keys.size();
}

void pump_mod_items(ModsPanel& panel) {
    auto& view = panel.items;
    free_retired(view);
    mods::ModItems items;
    {
        std::lock_guard lock(view.mutex);
        if (!view.result.ready) {
            if (!view.busy) start_read(panel);   // one asked for while another was being read
            return;
        }
        view.result.ready = false;
        if (view.result.mod != view.mod) return;   // another mod's overview is open by now
        items = std::move(view.result.items);
    }
    view.read = true;
    view.problem = std::move(items.problem);
    view.list.clear();
    view.categories.clear();
    release_atlases(view);
    // The thumbnails, a hundred to a texture: the launcher has few texture slots, a pack many items.
    std::vector<std::vector<unsigned char>> pixels;
    const auto side = thumbnail_side, width = side * atlas_columns;
    int cells = 0;
    for (auto& item : items.items) {
        ModItemsView::Item entry;
        entry.key = item.key;
        entry.title = item_title(item.key);
        entry.category = category_label(item.category);
        const auto& image = item.thumbnail;
        if (image.width == side && image.height == side && image.rgba.size() == static_cast<std::size_t>(side) * side * 4 &&
            static_cast<std::size_t>(cells / atlas_cells) < most_atlases) {
            if (cells % atlas_cells == 0) pixels.emplace_back(static_cast<std::size_t>(width) * width * 4, static_cast<unsigned char>(0));
            auto& atlas = pixels.back();
            const auto cell = cells % atlas_cells;
            const auto left = static_cast<std::size_t>(cell % atlas_columns) * side, top = static_cast<std::size_t>(cell / atlas_columns) * side;
            for (std::uint32_t row = 0; row < side; ++row)
                std::copy_n(image.rgba.begin() + static_cast<std::ptrdiff_t>(row) * side * 4, static_cast<std::size_t>(side) * 4,
                            atlas.begin() + static_cast<std::ptrdiff_t>(((top + row) * width + left) * 4));
            entry.cell = cells++;
        }
        auto found = std::find_if(view.categories.begin(), view.categories.end(), [&](const auto& one) { return one.first == entry.category; });
        if (found == view.categories.end()) found = view.categories.insert(view.categories.end(), {entry.category, 0});
        ++found->second;
        view.list.push_back(std::move(entry));
    }
    // A pack's items mostly begin alike ("wheelgraphic fm spitfire 00001"): a tile shows what tells
    // them apart, the words after the ones every item of its kind begins with. The whole name is
    // in the tile's tooltip and is what the search looks in.
    for (const auto& [category, count] : view.categories) {
        std::vector<ModItemsView::Item*> same;
        for (auto& item : view.list)
            if (item.category == category) same.push_back(&item);
        if (same.size() < 2) continue;
        std::size_t common = same.front()->title.size();
        for (const auto* item : same) {
            std::size_t at = 0;
            while (at < common && at < item->title.size() && item->title[at] == same.front()->title[at]) ++at;
            common = at;
        }
        const auto& first = same.front()->title;
        while (common > 0 && first[common - 1] != ' ') --common;   // whole words only
        if (!common || std::ranges::any_of(same, [&](const ModItemsView::Item* item) { return item->title.size() <= common; })) continue;
        for (auto* item : same) item->label = item->title.substr(common);
    }
    for (auto& item : view.list)
        if (item.label.empty()) item.label = item.title;
    std::stable_sort(view.list.begin(), view.list.end(), [](const ModItemsView::Item& a, const ModItemsView::Item& b) {
        return a.category != b.category ? a.category < b.category : a.title < b.title;
    });
    std::sort(view.categories.begin(), view.categories.end());
    if (g_renderer)
        for (const auto& atlas : pixels) view.atlases.push_back(g_renderer->upload_texture(atlas, width, width));
    // A choice made for a mod that turns out to add nothing is still shown, to be taken back.
    if (!view.problem.empty())
        logging::log(logging::Level::warning, logging::Channel::launcher, "{}: its items could not all be read: {}", view.mod, view.problem);
}

bool mod_items_known(const ModsPanel& panel, const mods::Mod& mod) {
    const auto& view = panel.items;
    return view.mod == mod.name && view.read && !view.list.empty();
}

std::string mod_items_count(const ModsPanel& panel, const mods::Mod& mod) {
    const auto& view = panel.items;
    if (!mod_items_known(panel, mod)) return {};
    const auto out = left_out(view);
    return out ? std::format("{} of {} load", view.list.size() - out, view.list.size()) : std::format("{}", view.list.size());
}

void mod_items_tab(const Fonts& fonts, ModsPanel& panel, const mods::Mod& mod, float height) {
    auto& view = panel.items;
    if (view.mod != mod.name) return;
    const float footer = ImGui::GetTextLineHeightWithSpacing() + S(6);

    // Search, the kinds of item, and All / None for what is shown (search "egg", None: no eggs).
    const auto query = folded_text(view.search.data());
    std::vector<ModItemsView::Item*> shown;
    for (auto& item : view.list) {
        if (!view.category.empty() && item.category != view.category) continue;
        if (!query.empty() && folded_text(item.title).find(query) == std::string::npos && item.key.find(query) == std::string::npos) continue;
        shown.push_back(&item);
    }
    ImGui::SetNextItemWidth(S(260));
    ImGui::InputTextWithHint("##item_search", "Search items", view.search.data(), view.search.size());
    if (view.categories.size() > 1) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(S(210));
        const auto current = view.category.empty() ? std::format("All kinds ({})", view.list.size()) : view.category;
        if (ImGui::BeginCombo("##item_kind", current.c_str())) {
            if (ImGui::Selectable(std::format("All kinds ({})", view.list.size()).c_str(), view.category.empty())) view.category.clear();
            for (const auto& [label, count] : view.categories)
                if (ImGui::Selectable(std::format("{} ({})", label, count).c_str(), view.category == label)) view.category = label;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    bool changed = false;
    ImGui::BeginDisabled(shown.empty());
    if (ImGui::Button("All", ImVec2(S(64), 0))) {
        for (const auto* item : shown) changed |= view.off.erase(item->key) > 0;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Load every item shown here.");
    ImGui::SameLine();
    if (ImGui::Button("None", ImVec2(S(64), 0))) {
        for (const auto* item : shown) changed |= view.off.insert(item->key).second;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Leave out every item shown here.");
    ImGui::EndDisabled();
    {
        const auto out = left_out(view);
        const auto text = std::format("{} of {} load", view.list.size() - out, view.list.size());
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - S(28) - ImGui::CalcTextSize(text.c_str()).x));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", text.c_str());
    }
    ImGui::Dummy(ImVec2(0, S(4)));

    ImGui::BeginChild("##mod_items", ImVec2(0, std::max(S(120), height - (ImGui::GetFrameHeightWithSpacing() + S(4)) - footer)),
                      ImGuiChildFlags_NavFlattened);
    if (!view.read) {
        ImGui::TextDisabled("Reading this mod's items...");
    } else if (shown.empty()) {
        ImGui::TextDisabled(view.list.empty() ? "This mod adds no items of its own." : "No item matches.");
    } else {
        auto* draw = ImGui::GetWindowDrawList();
        const float least_gap = S(10), picture = S(88), padding = S(8);
        const float tile_width = picture + padding * 2, tile_height = picture + padding * 2 + ImGui::GetTextLineHeight() * 2 + S(6);
        const float available = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(1, static_cast<int>((available + least_gap) / (tile_width + least_gap)));
        // The tiles spread over the width, so the grid ends where the page does.
        const float gap = columns > 1 ? std::min(S(24), (available - columns * tile_width) / (columns - 1)) : least_gap;
        const float uv = 1.0f / atlas_columns;
        int column = 0;
        std::string_view heading;
        for (auto* item : shown) {
            // Under its kind, when the mod has several and all are shown.
            if (view.category.empty() && view.categories.size() > 1 && item->category != heading) {
                heading = item->category;
                if (column) { ImGui::NewLine(); column = 0; }
                ImGui::Dummy(ImVec2(0, S(2)));
                ImGui::PushFont(fonts.bold);
                ImGui::TextUnformatted(item->category.c_str());
                ImGui::PopFont();
            }
            if (column) ImGui::SameLine(0, gap);
            ImGui::PushID(item->key.c_str());
            const bool on = !view.off.contains(item->key);
            const ImVec2 at = ImGui::GetCursorScreenPos(), end(at.x + tile_width, at.y + tile_height);
            if (ImGui::InvisibleButton("##tile", ImVec2(tile_width, tile_height))) {
                if (on) view.off.insert(item->key);
                else view.off.erase(item->key);
                changed = true;
            }
            const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", item->title.c_str(), item->key.c_str());
            if (ImGui::IsRectVisible(at, end)) {
                const float fade = on ? 1.0f : 0.32f;
                draw->AddRectFilled(at, end, on ? color::tile : rgba(22, 24, 29, 0.9f), S(6));
                const ImVec2 image(at.x + padding, at.y + padding), image_end(image.x + picture, image.y + picture);
                const auto atlas = item->cell >= 0 ? static_cast<std::size_t>(item->cell / atlas_cells) : view.atlases.size();
                if (atlas < view.atlases.size() && view.atlases[atlas]) {
                    const int cell = item->cell % atlas_cells;
                    const ImVec2 uv0((cell % atlas_columns) * uv, (cell / atlas_columns) * uv);
                    draw->AddImageRounded(view.atlases[atlas], image, image_end, uv0, ImVec2(uv0.x + uv, uv0.y + uv),
                                          rgba(255, 255, 255, fade), S(4));
                } else {
                    draw->AddRectFilled(image, image_end, rgba(255, 255, 255, 0.05f * fade), S(4));
                }
                // The name, on two lines at most.
                const ImVec2 text(at.x + padding, image_end.y + S(4));
                draw->PushClipRect(text, ImVec2(end.x - padding, text.y + ImGui::GetTextLineHeight() * 2), true);
                draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), text, on ? color::text : color::muted, item->label.c_str(), nullptr,
                              tile_width - padding * 2);
                draw->PopClipRect();
                // Ticked: it loads.
                const ImVec2 mark(end.x - S(15), at.y + S(15));
                if (on) {
                    draw->AddCircleFilled(mark, S(9), color::blue);
                    draw->AddLine(ImVec2(mark.x - S(4.5f), mark.y), ImVec2(mark.x - S(1.2f), mark.y + S(3.4f)), color::ink, S(2));
                    draw->AddLine(ImVec2(mark.x - S(1.2f), mark.y + S(3.4f)), ImVec2(mark.x + S(4.6f), mark.y - S(3.4f)), color::ink, S(2));
                } else {
                    draw->AddCircleFilled(mark, S(9), rgba(0, 0, 0, 0.55f));
                    draw->AddCircle(mark, S(9), color::muted, 0, S(1.5f));
                }
                if (hot) draw->AddRect(at, end, color::blue, S(6), 0, S(2));
            }
            ImGui::PopID();
            if (++column == columns) column = 0;
        }
    }
    ImGui::EndChild();

    if (!view.error.empty())
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(color::danger), "%s", view.error.c_str());
    else if (!view.problem.empty())
        ImGui::TextDisabled("Some of this mod could not be read, so not all of its items may be listed.");
    else
        ImGui::TextDisabled("An item left out is not loaded at all, nor what only it uses. A change is in the game the next time Skate starts.");
    if (changed) save(panel, mod);
}

} // namespace dingosdk::launcher_gui::detail
