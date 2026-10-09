#include "Engine/Game/World/world_names.h"
#include "Extension/UI/NativeMenu/native_tools_view.h"
#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>

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

void level_picker(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "travel", "FIND YOUR SPOT");
    std::vector<const Level*> destinations;
    const Level* root = nullptr;
    const Level* selected = nullptr;
    for (const auto& level : model.levels) {
        if (same(level.asset, root_asset)) root = &level;
        else {
            destinations.push_back(&level);
            if (same(level.asset, menu.destination)) selected = &level;
        }
    }
    std::sort(destinations.begin(), destinations.end(), [](const auto* a, const auto* b) {
        return level_name(*a) < level_name(*b);
    });
    field(menu, "Destination");
    if (ImGui::BeginCombo("##destination", selected ? level_name(*selected).c_str() : "Choose a level")) {
        for (const auto* level : destinations) {
            ImGui::PushID(level->asset.c_str());
            if (ImGui::Selectable(level_name(*level).c_str(), selected == level)) {
                selected = level;
                menu.destination = level->asset;
                menu.feedback.clear();
            }
            ImGui::PopID();
        }
        if (destinations.empty()) ImGui::TextDisabled("Waiting for levels...");
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, px(4)));
    const bool ready = root && selected && root->can_load && selected->can_load && model.can_queue_load && callbacks.queue_load;
    ImGui::BeginDisabled(!ready);
    skate_theme::push_primary_button();
    ImGui::PushFont(menu.heading);
    if (ImGui::Button("LET'S SKATE", ImVec2(-1, px(46)))) {
        std::array<char, 512> result{};
        const auto* point = selected->automatic_start_point();
        const bool queued = callbacks.queue_load(callbacks.user, root->asset.c_str(), "", selected->asset.c_str(),
            point ? point->c_str() : "", result.data(), result.size());
        result.back() = '\0';
        feedback(menu, result[0] ? result.data() : (queued ? "Loading level..." : "Load unavailable."));
    }
    ImGui::PopFont();
    skate_theme::pop_primary_button();
    ImGui::EndDisabled();
    if (!ready) {
        const std::string reason = !root ? "Waiting for levels..." : !selected ? "Choose a destination." :
            !root->can_load ? root->load_block_reason : !selected->can_load ? selected->load_block_reason : model.load_block_reason;
        note(reason.empty() ? "Level loading is unavailable." : reason.c_str());
    }
    end_card();
}

void park_editor_panel(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "park-editor", "PARK EDITOR");
    note("Build a park with freecam, an object library and XYZ handles, then save it as a mod "
         "others can install and load.");
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::BeginDisabled(!model.editor.available || !model.debug.camera_available || !model.debug.ui_available || !callbacks.queue_debug);
    skate_theme::push_primary_button();
    ImGui::PushFont(menu.heading);
    if (ImGui::Button("OPEN PARK EDITOR", ImVec2(-1, px(46))))
        debug_request(menu, callbacks, {DebugAction::set_park_editor, true});
    ImGui::PopFont();
    skate_theme::pop_primary_button();
    ImGui::EndDisabled();
    if (!model.editor.available) warn(model.editor.status.c_str());
    note("Placed objects are listed under Saved Objects.");
    end_card();
}

// Parks shared as mods (Mods/<name>/parks/<map>.park.json): one click loads
// a mod's park for this map in place of the current layout. In multiplayer
// only the host can, and the lobby sees the result.
void park_mods_panel(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const auto& editor = model.editor;
    begin_card(menu, "park-mods", "PARK MODS");
    note("Parks made in the park editor are saved as mods, so they can be shared and installed with the launcher. "
         "Loading one replaces your layout on this map; Undo in the park editor brings yours back.");
    if (!editor.can_load_parks) warn("In multiplayer only the host can load park mods.");
    else if (!editor.available) note(editor.status.c_str());
    end_card();
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
    if (here.empty())
        note(editor.map.empty() ? "Load a level to see its park mods."
                                : "No enabled park mod has a park for this map yet.");
    auto* draw = ImGui::GetWindowDrawList();
    for (std::size_t i = 0; i < here.size(); ++i) {
        const auto& mod = *here[i];
        ImGui::PushID(mod.folder.c_str());
        const auto top = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float buttons = px(210);
        const float text_width = width - buttons - px(36);
        const float description_height = mod.description.empty() ? 0.0f
            : ImGui::CalcTextSize(mod.description.c_str(), nullptr, false, text_width).y + px(4);
        const float height = std::max(px(74), px(58) + description_height);
        skate_theme::rough_rect(draw, top, ImVec2(top.x + width, top.y + height), skate_theme::tile,
                                static_cast<unsigned>(i + 71), px(1));
        if (mod.folder == editor.project)
            draw->AddRectFilled(top, ImVec2(top.x + px(4), top.y + height), blue);
        ImGui::SetCursorScreenPos(ImVec2(top.x + px(16), top.y + px(12)));
        ImGui::BeginGroup();
        ImGui::PushFont(menu.bold);
        ImGui::TextUnformatted(mod.title.empty() ? mod.folder.c_str() : mod.title.c_str());
        ImGui::PopFont();
        std::string byline = mod.author.empty() ? std::string("Unknown author") : "by " + mod.author;
        if (!mod.version.empty()) byline += "  /  v" + mod.version;
        if (mod.folder == editor.project) byline += "  /  open in the park editor";
        ImGui::TextDisabled("%s", byline.c_str());
        if (!mod.description.empty()) {
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + text_width);
            ImGui::TextUnformatted(mod.description.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(top.x + width - buttons - px(12), top.y + (height - ImGui::GetFrameHeight()) * .5f));
        ImGui::BeginDisabled(!ready || !editor.can_load_parks);
        skate_theme::push_primary_button();
        ImGui::PushFont(menu.bold);
        if (ImGui::Button("LOAD", ImVec2(px(100), 0)))
            send_console(menu, callbacks, command("mod-load", mod.folder));
        ImGui::PopFont();
        skate_theme::pop_primary_button();
        ImGui::SameLine();
        if (ImGui::Button("Edit", ImVec2(px(100), 0))) {
            // Open it as the editor's project, then show the editor.
            send_console(menu, callbacks, command("mod-open", mod.folder));
            debug_request(menu, callbacks, {DebugAction::set_park_editor, true});
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Load it into the park editor to change and save it.");
        ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + height + px(8)));
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
    ImGui::Dummy(ImVec2(0, px(4)));
    ImGui::BeginDisabled(!ready);
    if (ImGui::Button("Refresh park mods", ImVec2(-FLT_MIN, 0)))
        send_console(menu, callbacks, command("mods-refresh", {}));
    ImGui::EndDisabled();
    note("Disabled mods are hidden; enable them in the launcher's Mods panel.");
    if (!editor.status.empty() && editor.available) note(editor.status.c_str());
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
    ImGui::BeginDisabled(!state.available || !callbacks.queue_console_command || !state.can_clear);
    if (ImGui::Button(state.clearing ? "Clearing saved objects..." : "Delete saved objects for this level", ImVec2(-FLT_MIN, 0)))
        send("objects clear " + state.map);
    ImGui::EndDisabled();
    if (!state.status.empty()) note(state.status.c_str());
    end_card();
    section(menu, "PLACED OBJECTS");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##object-search", "Search objects...", menu.object_search.data(), menu.object_search.size());
    if (state.rows.empty()) {
        note("No saved or spawned objects in this level.");
        return;
    }
    std::string filter(menu.object_search.data());
    std::transform(filter.begin(), filter.end(), filter.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ImGui::BeginTable("placed-objects", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, std::max(50.0f, ImGui::GetContentRegionAvail().y - 2)))) {
        ImGui::TableSetupColumn("Object", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Saved + spawned").x + 4);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 112);
        ImGui::TableSetupScrollFreeze(0, 1); ImGui::TableHeadersRow();
        for (const auto& row : state.rows) {
            std::string label = row.item.starts_with("own_") ? row.item.substr(4) : row.item;
            std::replace(label.begin(), label.end(), '_', ' ');
            std::string searchable = label;
            std::transform(searchable.begin(), searchable.end(), searchable.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!filter.empty() && searchable.find(filter) == std::string::npos && row.item.find(filter) == std::string::npos) continue;
            const auto token = std::to_string(row.token);
            ImGui::PushID(token.c_str());
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nPosition: %.1f, %.1f, %.1f", row.item.c_str(), row.position[0], row.position[1], row.position[2]);
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(row.saved ? (row.spawned ? "Saved + spawned" : "Saved") : "Spawned");
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(!state.available || state.busy || !callbacks.queue_console_command);
            if (ImGui::Button("TP", ImVec2(34, 0))) send("objects tp " + state.map + " " + token);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Teleport beside this object's current or saved position.");
            ImGui::SameLine(0, 6);
            if (ImGui::Button("Delete", ImVec2(68, 0))) send("objects delete " + state.map + " " + token);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this object from the level and its saved layout.");
            ImGui::EndDisabled(); ImGui::PopID();
        }
        ImGui::EndTable();
    }
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
    ImGui::PushID("world-controls");
    ImGui::BeginDisabled(!controls.population_available || !model.world.ready || !callbacks.queue_console_command);
    for (unsigned i = 0; i < 2; ++i) {
        ImGui::PushID(static_cast<int>(i));
        const auto nearby = controls.population_ready[i] ? std::to_string(controls.population[i]) + " nearby right now"
                                                         : std::string("Waiting for the population count");
        field(menu, i == 0 ? "Traffic" : "Pedestrians", nearby.c_str());
        const int choice = i == 0 ? c.traffic : c.pedestrians;
        if (ImGui::BeginCombo("##population", population_labels[choice + 1])) {
            for (int mode = -1; mode <= 3; ++mode)
                if (ImGui::Selectable(population_labels[mode + 1], choice == mode)) send(i == 0 ? "traffic" : "pedestrians", static_cast<float>(mode));
            ImGui::EndCombo();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::PopID();
    std::string counts;
    for (unsigned i = 0; i < 2; ++i)
        if (controls.population_ready[i])
            counts += (counts.empty() ? "" : ", ") + std::to_string(controls.population[i]) + (i == 0 ? " vehicles" : " pedestrians");
    note(counts.empty() ? "Waiting for the population count." : ("Nearby now: " + counts + ".").c_str());
    end_card();
}

void parks(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    if (model.world.map == WorldMap::bam) {
        begin_card(menu, "rotating-parks", "ROTATING PARKS", "San Vansterdam");
        namespace tools = dingosdk::native_tools;
        note(tools::parks_locked(model) ? "The lobby host controls these layouts. Changes apply automatically."
             : model.parks.controlled_by_host ? "You are an admin of this server: a layout you load changes for everyone."
                                              : "Choose a layout for each slot. Choices save and are shared when you host.");
        note("Construction controls both the construction site and the Hedgemont park.");
        const auto park_command = [&](const char* command) {
            std::array<char, 512> result{};
            callbacks.queue_console_command(callbacks.user, command, result.data(), result.size());
            result.back() = '\0'; feedback(menu, result.data());
        };
        ImGui::BeginDisabled(!model.parks.available || tools::parks_locked(model) || !callbacks.queue_console_command);
        if (ImGui::Button("Load Random Parks")) {
            park_command("park random");
            menu.park_edit = {}; // Discard staged dropdown edits, including slots that reroll the same layout.
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!model.parks.available || !callbacks.queue_console_command);
        auto random_on_launch = model.parks.randomize_on_launch;
        if (ImGui::Checkbox("Randomize on Launch", &random_on_launch))
            park_command(random_on_launch ? "park random-on-launch 1" : "park random-on-launch 0");
        ImGui::EndDisabled();
        note("Pick a random layout for each slot. Randomize on Launch runs once per game launch; multiplayer follows the host.");
        ImGui::BeginDisabled(!model.parks.available || tools::parks_locked(model) || !callbacks.queue_console_command);
        if (ImGui::BeginTable("park-layouts", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("Layout", ImGuiTableColumnFlags_WidthStretch, 1.2f);
            ImGui::TableSetupColumn("##apply", ImGuiTableColumnFlags_WidthFixed, px(80));
            ImGui::TableHeadersRow();
            for (unsigned lot = 0; lot < park_lots.size(); ++lot) {
                ImGui::PushID(park_lots[lot].key.data());
                auto& selected = menu.park_edit[lot];
                if (selected.empty() || menu.park_seen[lot] != model.parks.choices[lot] || tools::parks_locked(model))
                    selected = model.parks.choices[lot];
                menu.park_seen[lot] = model.parks.choices[lot];
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(park_lots[lot].label.data());
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-1);
                if (ImGui::BeginCombo("##layout", park_label(selected).c_str())) {
                    if (ImGui::Selectable("Empty lot", selected == "empty")) selected = "empty";
                    for (unsigned family = 0; family < park_families.size(); ++family)
                        for (unsigned variant = 1; variant <= park_lots[lot].counts[family]; ++variant) {
                            const auto id = park_id(family, variant);
                            if (ImGui::Selectable(park_label(id).c_str(), selected == id)) selected = id;
                        }
                    ImGui::EndCombo();
                }
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(selected.empty());
                if (ImGui::Button("LOAD", ImVec2(-1, 0))) {
                    const auto command = "park " + std::string(park_lots[lot].key) + " " + selected;
                    std::array<char, 512> result{};
                    callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
                    result.back() = '\0'; feedback(menu, result.data());
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        if (!model.parks.ready) note("Load San Vansterdam to apply park choices.");
        if (!model.parks.feedback.empty()) note(model.parks.feedback.c_str());
        end_card();
    } else {
        begin_card(menu, "rotating-parks", "ROTATING PARKS");
        note("Rotating parks are on San Vansterdam. Switch maps in Travel.");
        end_card();
    }
}

void world_layers_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    if (model.world.controlled_by_host && model.multiplayer.server_admin)
        note("The server is syncing world layers. You are an admin, so your changes apply to everyone.");
    else if (model.world.controlled_by_host)
        warn("The host is syncing world layers. Your own choices come back when sync ends or you leave.");
    if (model.world.map == WorldMap::none) {
        note("Load a map to see its world layers.");
        return;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .61f);
    ImGui::InputTextWithHint("##layer-search", "Search layers...", menu.world_layer_search.data(), menu.world_layer_search.size());
    ImGui::SameLine(); ImGui::SetNextItemWidth(-1);
    constexpr const char* categories[]{"All categories", "Seasonal", "Activities", "World", "Lighting", "Structure", "Debug", "Parks"};
    ImGui::Combo("##layer-category", &menu.world_layer_category, categories, static_cast<int>(std::size(categories)));
    const std::string_view query(menu.world_layer_search.data());
    const auto matches_search = [&](std::string_view text) {
        return query.empty() || std::search(text.begin(), text.end(), query.begin(), query.end(), [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        }) != text.end();
    };
    ImGui::BeginDisabled(dingosdk::native_tools::layers_locked(model) || !model.world.available || !model.world.ready ||
                         !callbacks.queue_console_command);
    const float footer_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y +
        (model.world.feedback.empty() ? 0.0f : ImGui::CalcTextSize(model.world.feedback.c_str(), nullptr, false,
            ImGui::GetContentRegionAvail().x).y + ImGui::GetStyle().ItemSpacing.y);
    if (ImGui::BeginTable("world-layers", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
            ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
            ImVec2(0, std::max(95.0f, ImGui::GetContentRegionAvail().y - footer_height)))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 68);
        ImGui::TableHeadersRow();
        unsigned visible_layers{};
        for (unsigned i = 0; i < world_layers().size(); ++i) {
            const auto& layer = world_layers()[i];
            if (layer.map != model.world.map || !model.world.supported[i]) continue;
            if (menu.world_layer_category && layer.category != categories[menu.world_layer_category]) continue;
            if (!matches_search(layer.label) && !matches_search(layer.key)) continue;
            ++visible_layers;
            // Every row owns its checkbox ID, independent of table headers.
            ImGui::PushID(layer.key.data());
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextWrapped("%s", layer.label.data());
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
                ImGui::TextUnformatted(layer.detail.data());
                if (!model.world.status[i].empty()) ImGui::Text("%s", model.world.status[i].c_str());
                ImGui::PopTextWrapPos(); ImGui::EndTooltip();
            }
            ImGui::TableNextColumn();
            const bool follows_game = model.world.choices[i] == "default";
            bool enabled = follows_game ? model.world.enabled[i].value_or(false) : model.world.choices[i] == "on";
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight()));
            ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, follows_game && !model.world.enabled[i].has_value());
            if (ImGui::Checkbox("##enabled", &enabled)) {
                const auto command = "world " + std::string(layer.key) + (enabled ? " on" : " off");
                std::array<char, 512> result{};
                callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
                result.back() = '\0'; feedback(menu, result.data());
            }
            ImGui::PopItemFlag();
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26);
                ImGui::TextUnformatted(follows_game ? "Following the game's state." : "Saved override.");
                if (!model.world.enabled[i].has_value()) ImGui::TextUnformatted("Waiting for the layer's live state.");
                if (!model.world.status[i].empty()) ImGui::TextUnformatted(model.world.status[i].c_str());
                ImGui::TextUnformatted("Disabled parents take priority. Changes save automatically.");
                ImGui::PopTextWrapPos(); ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        if (!visible_layers) {
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::TextDisabled("No matching layers.");
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Restore Defaults", ImVec2(-1, 0))) {
        const auto command = "world defaults " + std::string(world_map_key(model.world.map));
        std::array<char, 512> result{};
        callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
        result.back() = '\0'; feedback(menu, result.data());
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Reset all layers on this map, including filtered rows,\nand return them to game control.");
    ImGui::EndDisabled();
    if (!model.world.feedback.empty()) note(model.world.feedback.c_str());
}

// One page per sidebar entry; each page's tabs switch between focused panels.
void map_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.map_tab, {"TRAVEL", "ROTATING PARKS", "WORLD LAYERS"}, "map-tabs");
    ImGui::PushID(menu.map_tab);
    ImGui::BeginChild("map-tab", ImVec2(0, page_body_height(menu)));
    switch (menu.map_tab) {
    case 0: level_picker(menu, model, callbacks); break;
    case 1: parks(menu, model, callbacks); break;
    case 2: world_layers_page(menu, model, callbacks); break;
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
    ImGui::Dummy(ImVec2(0, px(2)));
    ImGui::BeginDisabled(!graphics.available || !callbacks.queue_console_command);
    if (ImGui::Button("Restore defaults", ImVec2(-FLT_MIN, 0))) {
        command(menu, callbacks, "graphics reset -1");
        for (unsigned i = 0; i < labels.size(); ++i) {
            menu.graphics_pending[i] = -1; menu.graphics_pending_until[i] = ImGui::GetTime() + 3;
        }
    }
    ImGui::EndDisabled();
    note("Changes save automatically.");
    if (!graphics.status.empty()) note(graphics.status.c_str());
    end_card();
}

// The map's seven time-of-day world layers as one choice, like the native menu:
// forcing one turns the others off; Default hands all seven back to the level.
void time_of_day_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    namespace tod = dingosdk::native_tools;
    begin_card(menu, "time-of-day", "TIME OF DAY");
    const bool available = tod::time_of_day_available(model, callbacks);
    const int current = static_cast<int>(tod::time_of_day(model));
    const auto apply = [&](unsigned next) {
        for (const auto& line : tod::time_of_day_commands(model, next)) send_console(menu, callbacks, line);
    };
    int top = current < 4 ? current : -1;
    if (choice(menu, "tod-top", top, {"Default", "Morning", "Noon", "Afternoon"}, available)) apply(static_cast<unsigned>(top));
    int bottom = current >= 4 ? current - 4 : -1;
    if (choice(menu, "tod-bottom", bottom, {"Evening", "Night", "Weather day", "Weather night"}, available))
        apply(static_cast<unsigned>(bottom + 4));
    if (model.world.controlled_by_host && model.multiplayer.server_admin)
        note("You are an admin of this server: the time you pick applies to everyone.");
    else if (model.world.controlled_by_host) warn("The lobby host controls world layers, including time of day.");
    else if (model.world.map == WorldMap::none) note("Load a map in Map > Travel to change its time of day.");
    else if (!available) note("Time of day is not available on this map right now.");
    else note("Default follows the level's own time. Your choice saves and returns when you load this map again.");
    end_card();
}

void world_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.world_tab, {"TIME OF DAY", "ATMOSPHERE", "POPULATION"}, "world-tabs");
    ImGui::PushID(menu.world_tab);
    ImGui::BeginChild("world-tab", ImVec2(0, page_body_height(menu)));
    const bool loaded = model.world.map != WorldMap::none;
    switch (menu.world_tab) {
    case 0: time_of_day_page(menu, model, callbacks); break;
    case 1:
        if (loaded) atmosphere_menu(menu, model, callbacks);
        else note("Load a map in Map > Travel to change its fog, sky and wind.");
        break;
    case 2:
        if (loaded) population_controls(menu, model, callbacks);
        else note("Load a map in Map > Travel to change its traffic and pedestrians.");
        break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void build_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.build_tab, {"PARK EDITOR", "PARK MODS", "SAVED OBJECTS"}, "build-tabs");
    ImGui::PushID(menu.build_tab);
    ImGui::BeginChild("build-tab", ImVec2(0, page_body_height(menu)));
    if (menu.build_tab == 0) park_editor_panel(menu, model, callbacks);
    else if (menu.build_tab == 1) park_mods_panel(menu, model, callbacks);
    else saved_objects(menu, model, callbacks);
    ImGui::EndChild();
    ImGui::PopID();
}
}
