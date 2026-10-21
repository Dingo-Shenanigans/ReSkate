#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include "Extension/UI/NativeMenu/native_tools_view.h"
#include "Extension/Skater/drone_flight.h"

#include <algorithm>
#include <array>
#include <imgui_internal.h>
#include <string>

// The ADDONS page (what ReSkate adds to skate.: the drone, Board Break, Road Rash and Hall of Meat,
// music) and the full screen menu's HOME page.
namespace dingosdk::overlay::menu {
using namespace theme;
namespace {
// A tile for an addon on ALL ADDONS: what it is, whether it is on, and a way in.
void addon_tile(SkateMenu& menu, const char* id, const char* title, const char* about, const char* state, ImU32 state_colour,
                int addon) {
    if (console_style()) {
        // Full screen: a tile as the pause menu's, A opens the addon.
        const auto at = ImGui::GetCursorScreenPos();
        const ImVec2 size((ImGui::GetContentRegionAvail().x - px(12)) * .5f, px(168));
        ImGui::PushID(id);
        const bool pressed = ImGui::InvisibleButton("##addon", size, ImGuiButtonFlags_EnableNav);
        const bool lit = (ImGui::IsItemFocused() && ImGui::GetIO().NavVisible) || ImGui::IsItemHovered();
        if (lit) set_console_hint((std::string("Open ") + title + ".").c_str());
        auto* draw = ImGui::GetWindowDrawList();
        const ImVec2 end(at.x + size.x, at.y + size.y);
        skate_theme::rough_rect(draw, at, end, lit ? blue : IM_COL32(26, 26, 26, 235), static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, px(1));
        draw->PushClipRect(at, end, true);
        draw->AddText(menu.heading, px(26), ImVec2(at.x + px(20), at.y + px(18)), paper, title);
        if (state) {
            const auto text = menu.bold->CalcTextSizeA(px(13), FLT_MAX, 0, state);
            const ImVec2 tag_at(end.x - px(20) - text.x - px(14), at.y + px(22));
            draw->AddRectFilled(tag_at, ImVec2(tag_at.x + text.x + px(14), tag_at.y + text.y + px(8)), lit ? paper : state_colour);
            draw->AddText(menu.bold, px(13), ImVec2(tag_at.x + px(7), tag_at.y + px(4)), skate_theme::black, state);
        }
        draw->AddText(menu.body, px(16), ImVec2(at.x + px(20), at.y + px(66)), lit ? paper : muted, about, nullptr, size.x - px(40));
        draw->PopClipRect();
        ImGui::PopID();
        if (pressed) open_page(menu, page_addons, addon);
        return;
    }
    begin_card(menu, id, title);
    if (state) {
        ImGui::SameLine();
        tag(menu, state, state_colour);
    }
    note(about);
    ImGui::Dummy(ImVec2(0, px(2)));
    if (ImGui::Button((std::string("OPEN ") + title + "##" + id).c_str(), ImVec2(-FLT_MIN, px(34)))) open_page(menu, page_addons, addon);
    end_card();
}

void all_addons(SkateMenu& menu, const Model& model) {
    const auto on_off = [](bool available, bool on) -> const char* { return !available ? "NOT AVAILABLE" : on ? "ON" : "OFF"; };
    const auto colour = [](bool available, bool on) { return !available ? skate_theme::tile_light : on ? skate_theme::good : skate_theme::tile_light; };
    addon_tile(menu, "addon-drone", "DRONE", "Fly an FPV drone from the free camera, or let it follow you and film your line.", nullptr, 0,
               addon_drone);
    // Two a row full screen; the hidden ones (addon_hidden) leave no gap.
    int placed = 1;
    const auto next = [&] {
        if (console_style() && placed++ % 2) ImGui::SameLine(0, px(12));
    };
    const auto& bb = model.board_break;
    if (!addon_hidden(model, addon_board_break)) {
        next();
        addon_tile(menu, "addon-board-break", "BOARD BREAK", "Slams snap your board, boards wear out, and every crack has its sound.",
                   on_off(bb.available, bb.enabled), colour(bb.available, bb.enabled), addon_board_break);
    }
    const auto& rash = model.road_rash;
    if (!addon_hidden(model, addon_road_rash)) {
        next();
        addon_tile(menu, "addon-road-rash", "ROAD RASH", "Bails leave marks that build up: dust, scrapes, bruises, wounds.",
                   on_off(rash.available, rash.enabled), colour(rash.available, rash.enabled), addon_road_rash);
    }
    const auto& meat = model.hall_of_meat;
    next();
    addon_tile(menu, "addon-hall-of-meat", "HALL OF MEAT", "Your bails show the bones they hurt and score a Meat card.",
               on_off(meat.available, meat.enabled), colour(meat.available, meat.enabled), addon_hall_of_meat);
    const auto& grind = model.grind_exit;
    next();
    addon_tile(menu, "addon-grind-exits", "GRIND EXITS", "Press LT or RT while grinding to pick the side you pop out to.",
               on_off(grind.available, grind.enabled), colour(grind.available, grind.enabled), addon_grind_exits);
    next();
    addon_tile(menu, "addon-turnaround", "FIT CHECK", "Show off your boards, outfits and mods: press V on the Board or Skaters screen to save them spinning.",
               model.turnaround.busy ? "SAVING" : nullptr, skate_theme::good, addon_turnaround);
    next();
    addon_tile(menu, "addon-music", "MUSIC", "Spotify, YouTube Music, your own files and online radio through the game's music player.",
               nullptr, 0, addon_music);
}
}

bool addon_hidden(const Model& model, int addon) {
    return (addon == addon_board_break && model.board_break.off) || (addon == addon_road_rash && model.road_rash.off);
}

void addons_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    menu.addon = std::clamp(menu.addon, 0, static_cast<int>(addon_count) - 1);
    if (addon_hidden(model, menu.addon)) menu.addon = menu.fullscreen_drawn ? addon_all : addon_drone; // (a window has no overview)
    // A window lists the addons in its side rail; full screen has no rail, so they are tabs here
    // (LT / RT on a controller), without the hidden ones.
    if (menu.fullscreen_drawn) {
        static constexpr std::array<const char*, addon_count> names{"ALL ADDONS", "DRONE", "BOARD BREAK", "ROAD RASH", "HALL OF MEAT",
                                                                    "GRIND EXITS", "FIT CHECK", "MUSIC"};
        std::vector<const char*> labels;
        std::vector<int> shown;
        for (int a = 0; a < addon_count; ++a)
            if (!addon_hidden(model, a)) labels.push_back(names[static_cast<std::size_t>(a)]), shown.push_back(a);
        int tab = static_cast<int>(std::find(shown.begin(), shown.end(), menu.addon) - shown.begin());
        category_tabs(menu, tab, labels, "addon-tabs");
        menu.addon = shown[static_cast<std::size_t>(std::clamp(tab, 0, static_cast<int>(shown.size()) - 1))];
    }
    ImGui::PushID(menu.addon);
    switch (menu.addon) {
    case addon_drone: drone_page(menu, model, callbacks); break;
    case addon_music: music_page(menu, model, callbacks); break;
    default:
        ImGui::BeginChild("addon", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
        if (menu.addon == addon_board_break) board_break_addon(menu, model, callbacks);
        else if (menu.addon == addon_road_rash) road_rash_addon(menu, model, callbacks);
        else if (menu.addon == addon_hall_of_meat) hall_of_meat_addon(menu, model, callbacks);
        else if (menu.addon == addon_grind_exits) grind_exits_addon(menu, model, callbacks);
        else if (menu.addon == addon_turnaround) turnaround_addon(menu, model, callbacks);
        else all_addons(menu, model);
        ImGui::EndChild();
        break;
    }
    ImGui::PopID();
}

namespace {
// A HOME tile, in the pause menu's look: rough-cut, blue under the focus or the pointer. Its name
// at the top; what it is now (ON, OFF, the time of day) under it, or a line about the page it
// opens. Returns 1 when pressed (A, a click), -1 / 1 for left / right on a tile that `steps`.
int home_tile(SkateMenu& menu, const char* id, const char* name, const std::string& value, ImVec2 size, bool enabled, bool on,
              bool steps, const char* hint, bool big_value = true) {
    ImGui::BeginDisabled(!enabled);
    const auto at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    const unsigned seed = static_cast<unsigned>(ImGui::GetItemID()) & 0xfff;
    ImGui::EndDisabled();
    int result = 0;
    // A tile that steps is changed as the pause menu's settings are: A to start (it turns white),
    // then left / right, and A or B when done. Until then the D-pad moves past it. A click steps it on.
    static ImGuiID editing{};
    const ImGuiID item = ImGui::GetItemID();
    const bool mouse = pressed && hovered && ImGui::GetIO().MouseReleased[0];
    if (enabled && steps) {
        // (While it is held active ImGui makes no press of A, so A / B / Enter / Esc are read here.)
        if (editing == item && (!ImGui::IsItemFocused() || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false)))
            editing = 0;
        else if (pressed && !mouse) editing = item;
        else if (mouse) result = 1;
        if (editing == item) {
            result = console_step(true);
            // Held active while it is changed: B ends the change, not the page.
            ImGui::SetActiveID(item, ImGui::GetCurrentWindow());
        }
    } else if (enabled && pressed) {
        result = 1;
    }
    const bool changing = steps && editing == item;
    if (focused || hovered) set_console_hint(changing ? "Left / right to change it. A or B when done." : hint);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + size.x, at.y + size.y);
    const bool lit = enabled && (focused || hovered);
    skate_theme::rough_rect(draw, at, end, changing ? paper : lit ? blue : enabled ? IM_COL32(26, 26, 26, 235) : IM_COL32(26, 26, 26, 140), seed, px(1));
    draw->PushClipRect(at, end, true);
    draw->AddText(menu.bold, px(14), ImVec2(at.x + px(18), at.y + px(14)), changing ? skate_theme::black : lit ? paper : muted, name);
    const float value_size = big_value ? px(24) : px(15);
    const ImU32 value_ink = changing ? skate_theme::black : !enabled ? muted : lit ? paper : on ? blue : paper;
    draw->AddText(big_value ? menu.heading : menu.body, value_size, ImVec2(at.x + px(18), end.y - px(16) - value_size), value_ink, value.c_str(),
                  nullptr, size.x - px(36));
    if (steps && enabled) {
        // Left / right change it.
        const float s = px(6), mid = end.y - px(16) - value_size * .5f;
        const ImU32 arrow = changing ? skate_theme::black : lit ? paper : muted;
        draw->AddTriangleFilled(ImVec2(end.x - px(40), mid), ImVec2(end.x - px(40) + s, mid - s), ImVec2(end.x - px(40) + s, mid + s), arrow);
        draw->AddTriangleFilled(ImVec2(end.x - px(16), mid), ImVec2(end.x - px(16) - s, mid - s), ImVec2(end.x - px(16) - s, mid + s), arrow);
    }
    draw->PopClipRect();
    return result;
}
void heading(SkateMenu& menu, const char* text) {
    ImGui::PushFont(menu.heading);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, px(2)));
}
// A console line, without its reply on the page (a quick control says what it did by its tile).
void quietly(const CallbacksV3& callbacks, const std::string& command) {
    if (!callbacks.queue_console_command) return;
    std::array<char, 256> reply{};
    callbacks.queue_console_command(callbacks.user, command.c_str(), reply.data(), reply.size());
}
}

void home_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    ImGui::BeginChild("home", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    const float gap = px(12);
    const auto grid = [&](int columns) {
        return (ImGui::GetContentRegionAvail().x - gap * static_cast<float>(columns - 1)) / static_cast<float>(columns);
    };
    const float left = ImGui::GetCursorPosX();
    const auto place = [&](float width, int column) { ImGui::SameLine(left + (width + gap) * static_cast<float>(column)); };
    const auto& debug = model.debug;
    const auto on_off = [](bool value) { return std::string(value ? "ON" : "OFF"); };
    const auto request = [&](DebugAction action, bool value) {
        if (!callbacks.queue_debug) return;
        std::array<char, 256> reply{};
        callbacks.queue_debug(callbacks.user, {action, value}, reply.data(), reply.size());
    };

    // ---- the switches people reach for while skating
    heading(menu, "QUICK");
    {
        constexpr int columns = 5;
        const float width = grid(columns);
        const ImVec2 size(width, px(96));
        namespace tod = dingosdk::native_tools;
        static constexpr std::array<const char*, 8> times{"Default", "Morning", "Noon", "Afternoon", "Evening", "Night", "Weather day", "Weather night"};
        const bool tod_available = tod::time_of_day_available(model, callbacks);
        const int now = time_of_day_shown(model);
        const int step = home_tile(menu, "##time", "TIME OF DAY", times[static_cast<std::size_t>(std::clamp(now, 0, 7))], size, tod_available, now != 0,
                                   true, "Press A to change the time of day. Default follows the level's own.");
        // One time a press, on from the one last asked for.
        if (step) set_time_of_day(menu, model, callbacks, (now + step + 8) % 8, true);
        place(width, 1);
        if (home_tile(menu, "##noclip", "NOCLIP", on_off(debug.noclip), size, (debug.noclip_available || debug.noclip) && callbacks.queue_debug,
                      debug.noclip, false, "Fly with the normal player camera, through anything. Includes No Bail.") == 1)
            request(DebugAction::set_noclip, !debug.noclip);
        place(width, 2);
        if (home_tile(menu, "##no-bail", "NO BAIL", on_off(debug.no_bail), size, (debug.no_bail_available || debug.no_bail) && callbacks.queue_debug,
                      debug.no_bail, false, "Never wipe out.") == 1)
            request(DebugAction::set_no_bail, !debug.no_bail);
        place(width, 3);
        if (home_tile(menu, "##hide-hud", "HIDE HUD", on_off(debug.game_ui_hidden), size, debug.available && debug.ui_available && callbacks.queue_debug,
                      debug.game_ui_hidden, false, "Keep the view clear for riding and captures.") == 1)
            request(DebugAction::set_game_ui_hidden, !debug.game_ui_hidden);
        place(width, 4);
        if (home_tile(menu, "##first-person", "FIRST PERSON", on_off(debug.first_person), size, debug.available && callbacks.queue_debug,
                      debug.first_person, false, "See it through your skater's eyes.") == 1)
            request(DebugAction::set_first_person, !debug.first_person);
        ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));

        if (home_tile(menu, "##freecam", "FREECAM", on_off(debug.free_camera), size, debug.camera_available && callbacks.queue_debug,
                      debug.free_camera, false, "Leave your skater and fly the camera.") == 1)
            request(DebugAction::set_free_camera, !debug.free_camera);
        place(width, 1);
        const bool flying = drone::is_enabled() && debug.free_camera;
        if (home_tile(menu, "##drone", "DRONE", flying ? "FLYING" : "FLY IT", size, debug.camera_available && callbacks.queue_console_command, flying,
                      false, "Take off with the FPV drone, or land it. Its settings are in Addons > Drone.") == 1) {
            if (flying) quietly(callbacks, "dronemode 0"), quietly(callbacks, "freecam 0");
            else quietly(callbacks, "freecam 1"), quietly(callbacks, "dronemode 1");
        }
        // (The addons that are hidden, addon_hidden, leave no gap.)
        int column = 2;
        const auto& bb = model.board_break;
        if (!addon_hidden(model, addon_board_break)) {
            place(width, column++);
            if (home_tile(menu, "##board-break", "BOARD BREAK", on_off(bb.enabled), size, bb.available && callbacks.queue_console_command, bb.enabled,
                          false, "Slams snap your board. Its deck and settings are in Addons > Board Break.") == 1)
                quietly(callbacks, bb.enabled ? "boardbreak 0" : "boardbreak 1");
        }
        const auto& rash = model.road_rash;
        if (!addon_hidden(model, addon_road_rash)) {
            place(width, column++);
            if (home_tile(menu, "##road-rash", "ROAD RASH", on_off(rash.enabled), size, rash.available && callbacks.queue_console_command, rash.enabled,
                          false, "Bails leave marks that build up: dust, scrapes, bruises, wounds.") == 1)
                quietly(callbacks, rash.enabled ? "roadrash 0" : "roadrash 1");
        }
        place(width, column);
        const auto& meat = model.hall_of_meat;
        if (home_tile(menu, "##hall-of-meat", "HALL OF MEAT", on_off(meat.enabled), size, meat.available && callbacks.queue_console_command,
                      meat.enabled, false, "Your bails show the bones they hurt and score a Meat card.") == 1)
            quietly(callbacks, meat.enabled ? "hallofmeat 0" : "hallofmeat 1");
    }
    ImGui::Dummy(ImVec2(0, px(18)));

    // ---- the rest of the menu
    heading(menu, "GO TO");
    {
        constexpr int columns = 5;
        const float width = grid(columns);
        const ImVec2 size(width, px(96));
        const auto& mp = model.multiplayer;
        const std::string session = model.steam_offline ? "Offline: no multiplayer"
            : mp.active ? (mp.lobby_name.empty() ? std::string("In a session") : mp.lobby_name) + "  -  " + std::to_string(mp.players) + " / " +
                              std::to_string(mp.capacity)
                        : "Servers, hosting, voice";
        if (home_tile(menu, "##go-online", "ONLINE", session, ImVec2(width * 2 + gap, size.y), !model.steam_offline, mp.active, false,
                      "Join a server or a friend's lobby, host your own, voice chat.", false) == 1)
            open_page(menu, page_online);
        place(width, 2);
        if (home_tile(menu, "##go-map", "MAP", "Travel, parks, time and weather", size, true, false, false, "Pick a map and load it.", false) == 1)
            open_page(menu, page_map);
        place(width, 3);
        if (home_tile(menu, "##go-addons", "ADDONS", "Drone, Board Break, grind exits, music", size, true, false, false,
                      "What ReSkate adds to skate.", false) == 1)
            open_page(menu, page_addons, addon_all);
        place(width, 4);
        if (home_tile(menu, "##go-music", "MUSIC", "Now playing, queue, sources", size, true, false, false, "Your music in the game's player.", false) == 1)
            open_page(menu, page_addons, addon_music);
        ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
        if (home_tile(menu, "##go-skater", "SKATER", "Freecam, first person, cheats", size, true, false, false, "The free camera, first person, noclip and the boosts.", false) == 1)
            open_page(menu, page_skater);
        place(width, 1);
        if (home_tile(menu, "##go-sandbox", "SANDBOX", "Tuning, practice, HUD", size, true, false, false, "Tune the game's feel and drill your lines.", false) == 1)
            open_page(menu, page_sandbox);
        place(width, 2);
        if (home_tile(menu, "##go-build", "BUILD", "Park editor, park mods", size, true, false, false, "Make the park yours.", false) == 1)
            open_page(menu, page_build);
        place(width, 3);
        if (home_tile(menu, "##go-mods", "MODS", "Yours and the official ones", size, true, false, false, "The mods the game has loaded.", false) == 1)
            open_page(menu, page_mods);
        place(width, 4);
        if (home_tile(menu, "##go-settings", "SETTINGS", "Controls, screen, performance", size, true, false, false,
                      "Your binds, how the menu looks, the performance HUD.", false) == 1)
            open_page(menu, page_settings);
    }
    ImGui::EndChild();
}
}
