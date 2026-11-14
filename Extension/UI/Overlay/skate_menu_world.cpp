#include "Engine/Game/World/world_names.h"
#include "Extension/UI/NativeMenu/native_tools_view.h"
#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <utility>

// The MAP, WORLD and BUILD pages.
namespace dingosdk::overlay::menu {
using namespace theme;
constexpr char root_asset[] = "Levels/Game/DingoLevel_Root/DingoLevel_Root";

bool same(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) == std::tolower(y);
    });
}
std::string level_name(const std::string& asset) {
    return world_level_name(asset);
}
std::string level_name(const Level& level) {
    return level.display_name.empty() ? level_name(level.asset) : level.display_name;
}

// Which official map a level is, to mark the one loaded. The world layers know only those, so a
// custom map is marked only as a session's map.
WorldMap level_map(const Level& level) {
    auto name = world_level_short_name(level.asset);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "bam") return WorldMap::bam;
    if (name == "isle of grom") return WorldMap::grom;
    if (name == "ftue island") return WorldMap::ftue;
    if (name == "sdm int 001") return WorldMap::stadium_1;
    if (name == "sdm int 002") return WorldMap::stadium_2;
    if (name == "mpr") return WorldMap::mpr;
    return WorldMap::none;
}
bool level_loaded(const Model& model, const Level& level) {
    const auto session = world_destination_asset(model.multiplayer.map);
    if (model.multiplayer.active && !session.empty()) return same(level.asset, std::string(session));
    return model.world.map != WorldMap::none && level_map(level) == model.world.map;
}
std::string loaded_level_name(const Model& model) {
    for (const auto& level : model.levels)
        if (!same(level.asset, root_asset) && level_loaded(model, level)) return level_name(level);
    if (model.multiplayer.active && !model.multiplayer.map.empty()) return level_name(model.multiplayer.map);
    return model.world.map != WorldMap::none ? world_map_label(model.world.map) : std::string();
}

void level_picker(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    std::vector<const Level*> destinations;
    const Level* root = nullptr;
    for (const auto& level : model.levels) {
        if (same(level.asset, root_asset)) root = &level;
        // (Not the splash level: it only hosts the startup screen, as the pause menu's map list has it.
        // Nor Tutorial Island: the game's first-time tutorial, not a place to skate.)
        else if (dingosdk::native_tools::listed(level) && world_level_name(level.asset) != "Tutorial Island")
            destinations.push_back(&level);
    }
    std::sort(destinations.begin(), destinations.end(), [](const auto* a, const auto* b) {
        return level_name(*a) < level_name(*b);
    });
    {
        std::vector<MenuMapWanted> wanted;
        for (const auto* level : destinations) wanted.push_back({level->asset, level_name(*level)});
        want_menu_map_pictures(std::move(wanted));
    }
    const bool loading = root && root->can_load && model.can_queue_load && callbacks.queue_load;
    const auto load = [&](const Level* level) {
        std::array<char, 512> result{};
        const auto* point = level->automatic_start_point();
        const bool queued = callbacks.queue_load(callbacks.user, root->asset.c_str(), "", level->asset.c_str(),
            point ? point->c_str() : "", result.data(), result.size());
        result.back() = '\0';
        feedback(menu, result[0] ? result.data() : (queued ? ("Loading " + level_name(*level) + "...").c_str() : "Load unavailable."));
    };
    const std::string blocked = !root ? "Waiting for levels..." : !root->can_load ? root->load_block_reason : model.load_block_reason;
    if (!loading) warn(blocked.empty() ? "Level loading is unavailable." : blocked.c_str());

    // A box to find a map by name, for a keyboard; a controller passes over it to the cards.
    {
        static std::array<char, 64> search{};
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##map-search", "Search maps...", search.data(), search.size());
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        if (const auto words = lower(search.data()); !words.empty()) {
            std::erase_if(destinations, [&](const Level* level) { return lower(level_name(*level)).find(words) == std::string::npos; });
            if (destinations.empty()) note("No maps match that search.");
        }
    }

    // Which maps, and in what order. Newest installed puts the custom maps first, the mod installed
    // last on top, so a map just downloaded is found among many; the stock maps follow by name. When
    // each mod was installed comes with the pictures (map_icons.cpp), so nothing here reads the disk.
    static const std::vector<std::string> shows{"All", "Stock", "Custom"}, sorts{"A to Z", "Newest installed"};
    option_row(menu, "Show", "Left / right: every map, only the game's own, or only custom maps from mods.", menu.map_show, shows);
    option_row(menu, "Sort", "Left / right: by name, or custom maps by when their mod was installed, newest first.", menu.map_sort,
               sorts);
    const bool show_stock = menu.map_show != 2, show_custom = menu.map_show != 1, newest = menu.map_sort == 1;
    const auto installed = [](const Level* level) { return level->custom ? menu_map_installed(level->asset) : std::int64_t{}; };
    if (newest) {
        // (Stable, so maps whose time is not known yet, and the stock maps, keep their order by name.)
        std::vector<std::pair<std::int64_t, const Level*>> ordered;
        for (const auto* level : destinations) ordered.emplace_back(installed(level), level);
        std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (std::size_t i = 0; i < ordered.size(); ++i) destinations[i] = ordered[i].second;
    }
    // A custom map installed in the last week gets a NEW tag while they are sorted that way.
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    constexpr std::int64_t new_for = 7 * 24 * 60 * 60;
    if (!destinations.empty() && std::ranges::none_of(destinations, [&](const Level* level) { return level->custom ? show_custom : show_stock; }))
        note(show_custom ? "No custom maps to show." : "No stock maps to show.");

    // A card per map with its picture, as many to a row as fit; A (or a click) loads it. A map that
    // cannot load stays in reach, so a controller can still read why.
    auto* draw = ImGui::GetWindowDrawList();
    const auto grid = [&](const char* heading, bool custom) {
        std::vector<const Level*> shown;
        for (const auto* level : destinations)
            if (level->custom == custom) shown.push_back(level);
        if (shown.empty()) return;
        section(menu, heading);
        const float gap = px(12);
        const float across = ImGui::GetContentRegionAvail().x;
        const int columns = std::max(1, static_cast<int>((across + gap) / (px(240) + gap)));
        const float width = (across - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
        const float picture = width * 0.58f, height = picture + px(42);
        for (std::size_t i = 0; i < shown.size(); ++i) {
            const auto* level = shown[i];
            if (i % static_cast<std::size_t>(columns)) ImGui::SameLine(0, gap);
            ImGui::PushID(level->asset.c_str());
            const bool can = loading && level->can_load, current = level_loaded(model, *level);
            const auto at = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##map", ImVec2(width, height), ImGuiButtonFlags_EnableNav)) {
                if (can) load(level);
                else feedback(menu, !level->can_load ? level->load_block_reason.c_str() : blocked.c_str());
            }
            const bool hovered = ImGui::IsItemHovered();
            const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
            const std::string hint = !level->can_load ? level->load_block_reason
                                   : current          ? "Loaded now. A loads it again."
                                                      : "A loads " + level_name(*level) + ".";
            if (focused || hovered) set_console_hint(hint.c_str());
            if (!level->can_load && hovered) ImGui::SetTooltip("%s", level->load_block_reason.c_str());
            const ImVec2 end(at.x + width, at.y + height);
            draw->AddRectFilled(at, end, skate_theme::tile);
            ImTextureID texture{};
            ImVec2 uv0, uv1;
            if (menu_map_picture(level->asset, texture, uv0, uv1)) {
                // The middle band of the square picture, to the card's shape.
                const float band = (uv1.y - uv0.y) * (1.0f - picture / width) * 0.5f;
                draw->AddImage(texture, at, ImVec2(end.x, at.y + picture), ImVec2(uv0.x, uv0.y + band), ImVec2(uv1.x, uv1.y - band),
                               level->can_load ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 90));
            } else {
                draw->AddRectFilled(at, ImVec2(end.x, at.y + picture), skate_theme::tile_grey);
                const char* initial = custom ? "CUSTOM" : "MAP";
                const auto size = menu.bold->CalcTextSizeA(px(13), FLT_MAX, 0, initial);
                draw->AddText(menu.bold, px(13), ImVec2(at.x + (width - size.x) * .5f, at.y + (picture - size.y) * .5f), muted, initial);
            }
            draw->AddRectFilled(ImVec2(at.x, at.y + picture), end, focused ? focus_fill : hovered ? skate_theme::tile_light : skate_theme::tile);
            if (current) {
                // The map loaded now: a blue edge along its name and a badge on its picture.
                draw->AddRectFilled(ImVec2(at.x, at.y + picture), ImVec2(at.x + px(4), end.y), blue);
                const char* badge = "LOADED";
                const auto size = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, badge);
                const ImVec2 badge_at(at.x + px(8), at.y + px(8));
                draw->AddRectFilled(badge_at, ImVec2(badge_at.x + size.x + px(14), badge_at.y + size.y + px(6)), blue);
                draw->AddText(menu.bold, px(12), ImVec2(badge_at.x + px(7), badge_at.y + px(3)), paper, badge);
            }
            if (const auto when = newest && custom ? installed(level) : 0; when && now - when < new_for) {
                // Installed in the last week: a badge in the picture's other corner, clear of LOADED.
                const char* badge = "NEW";
                const auto size = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, badge);
                const ImVec2 badge_at(end.x - px(8) - size.x - px(14), at.y + px(8));
                draw->AddRectFilled(badge_at, ImVec2(badge_at.x + size.x + px(14), badge_at.y + size.y + px(6)), skate_theme::bar);
                draw->AddText(menu.bold, px(12), ImVec2(badge_at.x + px(7), badge_at.y + px(3)), skate_theme::black, badge);
            }
            draw->PushClipRect(at, end, true);
            draw->AddText(menu.bold, px(15), ImVec2(at.x + px(12), at.y + picture + px(12)),
                focused ? skate_theme::black : level->can_load ? paper : muted, level_name(*level).c_str());
            draw->PopClipRect();
            if (focused) draw->AddRect(ImVec2(at.x - px(3), at.y - px(3)), ImVec2(end.x + px(3), end.y + px(3)), paper, 0, 0, px(3));
            ImGui::PopID();
        }
    };
    if (newest) {
        if (show_custom) grid("CUSTOM MAPS", true);
        if (show_stock) grid("OFFICIAL MAPS", false);
    } else {
        if (show_stock) grid("OFFICIAL MAPS", false);
        if (show_custom) grid("CUSTOM MAPS", true);
    }
    if (destinations.empty()) note("Waiting for levels...");
}

void park_editor_panel(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "park-editor", "PARK EDITOR");
    note("Build a park with freecam, an object library and XYZ handles, then save it as a mod "
         "others can install and load.");
    const bool can = model.editor.available && model.debug.camera_available && model.debug.ui_available && callbacks.queue_debug;
    if (primary_button(menu, "OPEN PARK EDITOR", can)) debug_request(menu, callbacks, {DebugAction::set_park_editor, true});
    if (!model.editor.available) warn(model.editor.status.c_str());
    // The saved setting returns through the model a frame or more later: show the choice meanwhile.
    if (menu.editor_layout_pending &&
        (*menu.editor_layout_pending == model.park_editor_layout || ImGui::GetTime() > menu.editor_layout_until))
        menu.editor_layout_pending.reset();
    int layout = menu.editor_layout_pending.value_or(model.park_editor_layout) == park_editor_layout_rail ? 1 : 0;
    field(menu, "Layout");
    if (choice(menu, "editor-layout", layout, {"HOTBAR", "TOOL RAIL"}, callbacks.queue_console_command != nullptr, 0.0f,
            {"The world fills the screen, the objects in a hotbar along the bottom.",
             "A rail of tools down the left, a panel beside it and the selection on the right."})) {
        menu.editor_layout_pending = layout ? park_editor_layout_rail : park_editor_layout_hotbar;
        menu.editor_layout_until = ImGui::GetTime() + 3.0;
        send_console(menu, callbacks, layout ? "editor layout rail" : "editor layout hotbar");
    }
    note("Placed objects are listed under Saved Objects.");
    end_card();
}

// Parks shared as mods (Mods/<name>/parks/<map>.park.json): a row a mod with a park for this map,
// opening on LOAD (in place of the current layout) and EDIT. In multiplayer only the host can load
// one, and the lobby sees the result.
void park_mods_panel(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& editor = model.editor;
    begin_card(menu, "park-mods", "PARK MODS");
    note("Parks saved from the park editor as mods, shared and installed with the launcher. Loading one replaces "
         "your layout on this map; Undo in the park editor brings yours back.");
    if (!editor.can_load_parks) warn("In multiplayer only the host can load park mods.");
    else if (!editor.available) note(editor.status.c_str());
    const auto quoted = [](std::string_view text) {
        std::string out = "\"";
        for (const auto c : text) {
            if (c == '"' || c == '\\') out += '\\';
            out += c;
        }
        return out + '"';
    };
    const auto command = [&](const char* operation, const std::string& folder) {
        return std::string("editor ") + operation + " " + quoted(editor.map) + " " + std::to_string(editor.generation) +
               " " + std::to_string(editor.revision) + (folder.empty() ? "" : " " + quoted(folder));
    };
    const bool ready = editor.available && !editor.busy && !editor.failed && callbacks.queue_console_command;
    std::vector<const EditorParkMod*> here, elsewhere;
    for (const auto& mod : editor.park_mods)
        if (mod.enabled) (mod.has_map ? here : elsewhere).push_back(&mod);
    section(menu, "FOR THIS MAP");
    if (here.empty())
        note(editor.map.empty() ? "Load a level to see its park mods."
                                : "No enabled park mod has a park for this map yet.");
    static std::string open_mod;
    for (const auto* mod : here) {
        ImGui::PushID(mod->folder.c_str());
        std::string detail = mod->author.empty() ? std::string("Unknown author") : "by " + mod->author;
        if (!mod->version.empty()) detail += "  /  v" + mod->version;
        if (!mod->description.empty()) detail += "\n" + mod->description;
        const bool in_editor = mod->folder == editor.project;
        if (expand_row(menu, mod->folder.c_str(), mod->title.empty() ? mod->folder : mod->title, detail, in_editor ? "IN THE PARK EDITOR" : "",
                       open_mod, in_editor)) {
            const bool can = ready && editor.can_load_parks;
            const int pressed = row_actions(menu, "actions", {{"LOAD", can, true, "Replace this map's layout with this park."},
                                                              {"EDIT", can, false, "Load it into the park editor to change and save it."}});
            if (pressed == 0) send_console(menu, callbacks, command("mod-load", mod->folder));
            if (pressed == 1) {
                // Open it as the editor's project, then show the editor.
                send_console(menu, callbacks, command("mod-open", mod->folder));
                debug_request(menu, callbacks, {DebugAction::set_park_editor, true});
            }
        }
        ImGui::PopID();
    }
    if (!elsewhere.empty()) {
        std::string names;
        for (const auto* mod : elsewhere) {
            std::string maps;
            for (const auto& map : mod->maps) maps += (maps.empty() ? "" : ", ") + map;
            names += (names.empty() ? "" : "; ") + (mod->title.empty() ? mod->folder : mod->title) + " (" + maps + ")";
        }
        note(("For other maps: " + names).c_str());
    }
    if (button_row(menu, "park-mods-refresh", {{"REFRESH PARK MODS", ready, false, "Read the installed park mods again."}}) == 0)
        send_console(menu, callbacks, command("mods-refresh", {}));
    note("Disabled mods are hidden; enable them in the launcher's Mods panel.");
    if (!editor.status.empty() && editor.available) note(editor.status.c_str());
    end_card();
}

void saved_objects(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& state = model.object_persistence;
    const auto send = [&](const std::string& command) {
        std::array<char, 512> result{};
        callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
        result.back() = '\0'; feedback(menu, result.data());
    };
    begin_card(menu, "persistence", "OBJECT PERSISTENCE");
    bool enabled = state.enabled;
    if (toggle_row(menu, "Save and restore placed objects",
            "Off pauses saving and restoring. Existing saved layouts are kept.", enabled,
            state.available && callbacks.queue_console_command))
        send(enabled ? "objects enabled 1" : "objects enabled 0");
    if (!state.map.empty())
        info(menu, "Current level", std::string(world_map_label(model.world.map)) + ", " + std::to_string(state.count) + " saved");
    else note("Load a level to manage its saved objects.");
    const bool can_clear = state.available && callbacks.queue_console_command && state.can_clear;
    if (button_row(menu, "objects-clear", {{state.clearing ? "CLEARING SAVED OBJECTS..." : "DELETE THIS LEVEL'S SAVED OBJECTS", can_clear, false,
                                           "Removes every saved object of this level."}}) == 0)
        send("objects clear " + state.map);
    if (!state.status.empty()) note(state.status.c_str());
    end_card();

    // Each object a row; A opens TELEPORT and DELETE under it.
    begin_card(menu, "placed-objects", "PLACED OBJECTS");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##object-search", "Search objects...", menu.object_search.data(), menu.object_search.size());
    if (state.rows.empty()) note("No saved or spawned objects in this level.");
    std::string filter(menu.object_search.data());
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const bool can = state.available && !state.busy && callbacks.queue_console_command;
    static std::string open_object;
    unsigned shown{};
    for (const auto& row : state.rows) {
        std::string label = row.item.starts_with("own_") ? row.item.substr(4) : row.item;
        std::replace(label.begin(), label.end(), '_', ' ');
        std::string searchable = label;
        std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!filter.empty() && searchable.find(filter) == std::string::npos && row.item.find(filter) == std::string::npos) continue;
        ++shown;
        const auto token = std::to_string(row.token);
        ImGui::PushID(token.c_str());
        if (expand_row(menu, token.c_str(), label, "", row.saved ? (row.spawned ? "Saved + spawned" : "Saved") : "Spawned", open_object)) {
            char where[160]{};
            ImFormatString(where, sizeof(where), "%s at %.1f, %.1f, %.1f", row.item.c_str(), row.position[0], row.position[1], row.position[2]);
            ImGui::Indent(px(40));
            note(where);
            ImGui::Unindent(px(40));
            const int pressed = row_actions(menu, "actions", {{"TELEPORT", can, true, "Teleport beside this object's current or saved position."},
                                                              {"DELETE", can, false, "Remove this object from the level and its saved layout."}});
            if (pressed == 0) send("objects tp " + state.map + " " + token);
            if (pressed == 1) send("objects delete " + state.map + " " + token);
        }
        ImGui::PopID();
    }
    if (!state.rows.empty() && !shown) note("No objects match that search.");
    end_card();
}

void population_controls(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& controls = model.world_controls;
    const auto& c = controls.choices;
    const auto send = [&](const char* key, float value) {
        const auto command = "environment " + std::string(key) + " " + std::to_string(value);
        std::array<char, 512> result{};
        const bool queued = callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
        result.back() = '\0'; feedback(menu, result.data());
        return queued;
    };
    begin_card(menu, "population", "POPULATION");
    const bool can = controls.population_available && model.world.ready && callbacks.queue_console_command;
    std::vector<std::string> modes;
    for (const auto* label : population_labels) modes.emplace_back(label);
    for (unsigned i = 0; i < 2; ++i) {
        int chosen = (i == 0 ? c.traffic : c.pedestrians) + 1;
        const auto nearby = controls.population_ready[i] ? std::to_string(controls.population[i]) + " nearby right now."
                                                         : std::string("Waiting for the population count.");
        if (option_row(menu, i == 0 ? "Traffic" : "Pedestrians", nearby.c_str(), chosen, modes, can))
            send(i == 0 ? "traffic" : "pedestrians", static_cast<float>(chosen - 1));
    }
    std::string counts;
    for (unsigned i = 0; i < 2; ++i)
        if (controls.population_ready[i])
            counts += (counts.empty() ? "" : ", ") + std::to_string(controls.population[i]) + (i == 0 ? " vehicles" : " pedestrians");
    note(counts.empty() ? "Waiting for the population count." : ("Nearby now: " + counts + ".").c_str());
    end_card();
}

void parks(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    namespace tools = dingosdk::native_tools;
    if (model.world.map != WorldMap::bam) {
        begin_card(menu, "rotating-parks", "ROTATING PARKS");
        note("Rotating parks are on San Vansterdam. Switch maps in Travel.");
        end_card();
        return;
    }
    const auto park_command = [&](const char* command) {
        std::array<char, 512> result{};
        callbacks.queue_console_command(callbacks.user, command, result.data(), result.size());
        result.back() = '\0'; feedback(menu, result.data());
    };
    const bool locked = !model.parks.available || tools::parks_locked(model) || !callbacks.queue_console_command;
    // A row a slot: left / right picks its layout, A loads it.
    begin_card(menu, "rotating-parks", "ROTATING PARKS", "San Vansterdam");
    note(tools::parks_locked(model) ? "The lobby host controls these layouts. Changes apply automatically."
         : model.parks.controlled_by_host ? "You are an admin of this server: a layout you load changes for everyone."
                                          : "Pick a layout with left / right, then A loads it. Choices save and are shared when you host.");
    for (unsigned lot = 0; lot < park_lots.size(); ++lot) {
        auto& staged = menu.park_edit[lot];
        if (staged.empty() || menu.park_seen[lot] != model.parks.choices[lot] || tools::parks_locked(model))
            staged = model.parks.choices[lot];
        menu.park_seen[lot] = model.parks.choices[lot];
        std::vector<std::string> ids{"empty"}, names{"Empty lot"};
        for (unsigned family = 0; family < park_families.size(); ++family)
            for (unsigned variant = 1; variant <= park_lots[lot].counts[family]; ++variant) {
                ids.push_back(park_id(family, variant));
                names.push_back(park_label(ids.back()));
            }
        int at = -1;
        for (std::size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == staged) at = static_cast<int>(i);
        const bool loaded = staged == model.parks.choices[lot];
        const auto label = std::string(park_lots[lot].label) + (loaded ? "" : "  (A loads)") + "###" + std::string(park_lots[lot].key);
        bool load = false;
        if (option_row(menu, label.c_str(), loaded ? "Left / right to pick a layout, then A to load it." : "Not loaded yet: press A to load it.",
                       at, names, !locked, &load) && at >= 0)
            staged = ids[static_cast<std::size_t>(at)];
        if (load && !staged.empty())
            park_command(("park " + std::string(park_lots[lot].key) + " " + staged).c_str());
    }
    note("Construction is both the construction site and the Hedgemont park.");
    if (!model.parks.ready) note("Load San Vansterdam to apply park choices.");
    if (!model.parks.feedback.empty()) note(model.parks.feedback.c_str());
    end_card();

    begin_card(menu, "random-parks", "RANDOM PARKS");
    if (button_row(menu, "park-random", {{"LOAD RANDOM PARKS", !locked, false, "A random layout in every slot."}}) == 0) {
        park_command("park random");
        menu.park_edit = {}; // Drop the layouts picked and not loaded, including slots that reroll the same layout.
    }
    auto random_on_launch = model.parks.randomize_on_launch;
    if (toggle_row(menu, "Randomize on launch", "A random layout in every slot once each time the game starts. Multiplayer follows the host.",
                   random_on_launch, model.parks.available && callbacks.queue_console_command))
        park_command(random_on_launch ? "park random-on-launch 1" : "park random-on-launch 0");
    end_card();
}

void world_layers_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "world-layers", "WORLD LAYERS");
    if (model.world.controlled_by_host && server_can(model.multiplayer, "layers"))
        note("The server is syncing world layers. Your role may change them, so your changes apply to everyone.");
    else if (model.world.controlled_by_host)
        warn("The host is syncing world layers. Your own choices come back when sync ends or you leave.");
    if (model.world.map == WorldMap::none) {
        note("Load a map in Map > Travel to see its world layers.");
        end_card();
        return;
    }
    // Which kind, a search box, then a switch for each layer.
    static const std::vector<std::string> kinds{"All categories", "Seasonal", "Activities", "World", "Lighting", "Structure", "Debug", "Parks"};
    option_row(menu, "Show", "Left / right for one kind of layer.", menu.world_layer_category, kinds);
    // A box to find a layer by name, for a keyboard; a controller passes over it.
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##layer-search", "Search layers...", menu.world_layer_search.data(), menu.world_layer_search.size());
    const std::string_view query(menu.world_layer_search.data());
    const auto found = [&](std::string_view text) {
        return query.empty() || std::search(text.begin(), text.end(), query.begin(), query.end(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }) != text.end();
    };
    const bool can = !dingosdk::native_tools::layers_locked(model) && model.world.available && model.world.ready && callbacks.queue_console_command;
    unsigned shown{};
    for (unsigned i = 0; i < world_layers().size(); ++i) {
        const auto& layer = world_layers()[i];
        if (layer.map != model.world.map || !model.world.supported[i] || world_layer_kept_off(layer.key)) continue;
        if (menu.world_layer_category && layer.category != kinds[static_cast<std::size_t>(menu.world_layer_category)]) continue;
        if (!found(world_layer_title(layer.label)) && !found(layer.key)) continue;
        ++shown;
        const bool follows_game = model.world.choices[i] == "default";
        bool enabled = follows_game ? model.world.enabled[i].value_or(false) : model.world.choices[i] == "on";
        // What the layer is, then whether the game or a saved choice has it, and its live state's problem if any.
        std::string hint(layer.detail);
        if (!follows_game) hint += " Saved override.";
        if (!model.world.status[i].empty()) hint += " " + model.world.status[i];
        ImGui::PushID(layer.key.data());
        if (toggle_row(menu, world_layer_title(layer.label).c_str(), hint.c_str(), enabled, can))
            send_console(menu, callbacks, "world " + std::string(layer.key) + (enabled ? " on" : " off"));
        ImGui::PopID();
    }
    if (!shown) note("No layers of that kind on this map.");
    note("Disabled parents take priority. Changes save automatically.");
    if (button_row(menu, "layer-defaults", {{"RESTORE DEFAULTS", can, false, "Every layer on this map back to the game's control."}}) == 0)
        send_console(menu, callbacks, "world defaults " + std::string(world_map_key(model.world.map)));
    if (!model.world.feedback.empty()) note(model.world.feedback.c_str());
    end_card();
}

void time_of_day_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks);

// One page per sidebar entry; each page's tabs switch between focused panels.
void map_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.map_tab, {"TRAVEL", "PARKS", "TIME AND WEATHER", "LAYERS AND CROWDS"}, "map-tabs");
    ImGui::PushID(menu.map_tab);
    ImGui::BeginChild("map-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    const bool loaded = model.world.map != WorldMap::none;
    switch (menu.map_tab) {
    case 0: level_picker(menu, model, callbacks); break;
    case 1: parks(menu, model, callbacks); break;
    case 2:
        time_of_day_page(menu, model, callbacks);
        if (loaded) {
            atmosphere_menu(menu, model, callbacks);
        } else {
            begin_card(menu, "atmosphere", "FOG, SKY AND WIND");
            note("Load a map in Map > Travel to change its fog, sky and wind.");
            end_card();
        }
        break;
    case 3:
        world_layers_page(menu, model, callbacks);
        if (loaded) {
            population_controls(menu, model, callbacks);
        } else {
            begin_card(menu, "population", "POPULATION");
            note("Load a map in Map > Travel to change its traffic and pedestrians.");
            end_card();
        }
        break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void graphics_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "post-fx", "POST PROCESSING");
    const auto command = [](SkateMenu& target, const CallbacksV3& cb, const std::string& text) {
        std::array<char, 512> result{};
        cb.queue_console_command(cb.user, text.c_str(), result.data(), result.size());
        result.back() = '\0';
        feedback(target, result.data());
    };
    constexpr std::array<const char*, 3> labels{"Film grain", "Vignette", "Chromatic aberration"};
    constexpr std::array<const char*, 3> hints{
        "Grain over the picture. On allows the scene's authored grain amount.",
        "Darkening around the edges of the picture.",
        "Color fringing around the edges of the picture."};
    const auto& graphics = model.graphics;
    for (unsigned i = 0; i < labels.size(); ++i) {
        auto& pending = menu.graphics_pending[i];
        if (pending && (*pending == graphics.choices.effects[i] || ImGui::GetTime() > menu.graphics_pending_until[i] ||
            graphics.status == "Graphics controls could not be saved.")) pending.reset();
        const int choice = pending.value_or(graphics.choices.effects[i]);
        bool enabled = choice < 0 ? (graphics.ready[i] ? graphics.enabled[i] : true) : choice != 0;
        if (toggle_row(menu, labels[i], hints[i], enabled, graphics.available && callbacks.queue_console_command)) {
            const int value = enabled ? 1 : 0;
            command(menu, callbacks, std::string("graphics ") + graphics_keys[i] + " " + std::to_string(value));
            pending = value; menu.graphics_pending_until[i] = ImGui::GetTime() + 3;
        }
    }
    if (button_row(menu, "post-fx-defaults", {{"RESTORE DEFAULTS", graphics.available && callbacks.queue_console_command, false,
                                               "Every effect back to what the scene asks for."}}) == 0) {
        command(menu, callbacks, "graphics reset -1");
        for (unsigned i = 0; i < labels.size(); ++i) {
            menu.graphics_pending[i] = -1; menu.graphics_pending_until[i] = ImGui::GetTime() + 3;
        }
    }
    note("Changes save automatically.");
    if (!graphics.status.empty()) note(graphics.status.c_str());
    end_card();
}

// The map's seven time-of-day world layers as one choice, like the native menu:
// forcing one turns the others off; Default hands all seven back to the level.
// A time of day is seven layer changes the game takes a few frames to make, and until they are
// all made the layers read as some other time (or none). So the time asked for is kept and shown,
// and the next step goes on from it, until the layers say it or four seconds pass.
// Stepping through the times only moves the choice: it goes to the game once the stepping has
// stopped for tod_settle seconds. Each time loads its weather, and a player stepping from Default
// to Night made the game load every one on the way (a stall each, on a slower PC).
namespace {
int tod_asked = -1;
double tod_asked_until = 0;
int tod_pending = -1;     // the time chosen and not yet sent
double tod_pending_at = 0;
CallbacksV3 tod_callbacks{};
constexpr double tod_settle = 0.6;
}
int time_of_day_shown(const Model& model) {
    if (tod_pending >= 0 && ImGui::GetTime() >= tod_pending_at + tod_settle) {
        tod_asked = std::exchange(tod_pending, -1);
        tod_asked_until = ImGui::GetTime() + 4.0;
        if (tod_callbacks.queue_console_command)
            for (const auto& line : dingosdk::native_tools::time_of_day_commands(model, static_cast<unsigned>(tod_asked))) {
                std::array<char, 256> reply{};
                tod_callbacks.queue_console_command(tod_callbacks.user, line.c_str(), reply.data(), reply.size());
            }
    }
    if (tod_pending >= 0) return tod_pending;
    const int now = static_cast<int>(dingosdk::native_tools::time_of_day(model));
    if (tod_asked >= 0 && (now == tod_asked || ImGui::GetTime() > tod_asked_until)) tod_asked = -1;
    return tod_asked >= 0 ? tod_asked : now;
}
void set_time_of_day(SkateMenu&, const Model&, const CallbacksV3& callbacks, int next, bool) {
    tod_pending = std::clamp(next, 0, 7);
    tod_pending_at = ImGui::GetTime();
    tod_callbacks = callbacks;
}

void time_of_day_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    namespace tod = dingosdk::native_tools;
    begin_card(menu, "time-of-day", "TIME OF DAY");
    const bool available = tod::time_of_day_available(model, callbacks);
    static const std::vector<std::string> times{"Default", "Morning", "Noon", "Afternoon", "Evening", "Night", "Weather day", "Weather night"};
    int chosen = time_of_day_shown(model);
    if (option_row(menu, "Time of day", "Left / right to change it. Default follows the level's own time.", chosen, times, available))
        set_time_of_day(menu, model, callbacks, chosen, false);
    if (model.world.controlled_by_host && server_can(model.multiplayer, "tod"))
        note("Your role on this server sets the time: the one you pick applies to everyone.");
    else if (model.world.controlled_by_host) warn("The lobby host controls world layers, including time of day.");
    else if (model.world.map == WorldMap::none) note("Load a map in Map > Travel to change its time of day.");
    else if (!available) note("Time of day is not available on this map right now.");
    else note("Default follows the level's own time. Your choice saves and returns when you load this map again.");
    end_card();
}

void build_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.build_tab, {"PARK EDITOR", "PARK MODS", "SAVED OBJECTS"}, "build-tabs");
    ImGui::PushID(menu.build_tab);
    ImGui::BeginChild("build-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    if (menu.build_tab == 0) park_editor_panel(menu, model, callbacks);
    else if (menu.build_tab == 1) park_mods_panel(menu, model, callbacks);
    else saved_objects(menu, model, callbacks);
    ImGui::EndChild();
    ImGui::PopID();
}
}
