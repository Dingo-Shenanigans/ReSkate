#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include "Extension/UI/NativeMenu/native_tools_view.h"
#include "Extension/Plugins/plugin_runtime.h"
#include "modding_menu.h"

#include <algorithm>
#include <array>
#include <imgui_internal.h>
#include <string>

// The menu's HOME page: where the player is, the quick switches, and the pages to pick up again.
namespace dingosdk::overlay::menu {
using namespace theme;
namespace {
// A HOME tile, in the menu kit's look: rough-cut, white with dark text under the focus, a lighter
// grey under the pointer. Its name at the top; what it is now (ON, OFF, the time of day) under it,
// or a line about the page it opens. Returns 1 when pressed (A, a click), -1 / 1 for left / right on
// a tile that `steps`.
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
    // The kit's greys (skate_menu.cpp): a row's fill, and a step lighter under the pointer. Being
    // changed it stays the focus blue, with a black bar along its foot.
    const bool lit = enabled && (focused || changing);
    const ImU32 fill = lit ? focus_fill : !enabled ? IM_COL32(40, 40, 43, 130) : hovered ? IM_COL32(56, 57, 61, 255) : IM_COL32(40, 40, 43, 255);
    tile_rect(draw, at, end, fill, seed, px(1));
    if (changing) draw->AddRectFilled(ImVec2(at.x, end.y - px(4)), end, skate_theme::black);
    const ImU32 focus_text = IM_COL32(11, 11, 11, 255);
    draw->PushClipRect(at, end, true);
    draw->AddText(menu.bold, px(14), ImVec2(at.x + px(18), at.y + px(14)), lit ? IM_COL32(10, 40, 82, 255) : muted, name);
    // The value on one line: a size smaller when the tile is too narrow for it (the side sheet).
    const float room = size.x - px(36) - (steps && enabled ? px(40) : 0.0f);
    ImFont* value_font = big_value ? menu.heading : menu.body;
    float value_size = big_value ? px(24) : px(15);
    if (big_value && value_font->CalcTextSizeA(value_size, FLT_MAX, 0, value.c_str()).x > room) value_size = px(18);
    const ImU32 value_ink = lit ? focus_text : !enabled ? muted : on ? blue : paper;
    draw->PushClipRect(at, ImVec2(at.x + px(18) + room, end.y), true);
    draw->AddText(value_font, value_size, ImVec2(at.x + px(18), end.y - px(16) - value_size), value_ink, value.c_str());
    draw->PopClipRect();
    if (steps && enabled) {
        // Left / right change it.
        const float s = px(6), mid = end.y - px(16) - value_size * .5f;
        const ImU32 arrow = lit ? focus_text : hovered ? paper : muted;
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
}

namespace {
// A chip of the YOU ARE ON card: white under the focus, a darker blue than the card otherwise.
bool where_chip(SkateMenu& menu, const char* label, float width, const char* hint) {
    const auto at = ImGui::GetCursorScreenPos();
    const ImVec2 size(width, px(34));
    const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
    const bool hovered = ImGui::IsItemHovered(), lit = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
    if (lit || hovered) set_console_hint(hint);
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 end(at.x + size.x, at.y + size.y);
    tile_rect(draw, at, end, lit ? focus_fill : hovered ? IM_COL32(246, 246, 248, 255) : IM_COL32(226, 227, 230, 255),
                            static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, px(1));
    const auto text = menu.bold->CalcTextSizeA(px(14), FLT_MAX, 0, label);
    draw->AddText(menu.bold, px(14), ImVec2(at.x + (size.x - text.x) * .5f, at.y + (size.y - text.y) * .5f),
                  IM_COL32(11, 11, 11, 255), label);
    return pressed;
}

// Where the player is: in a session, its name, then the map, the time, how many skate there and how many
// are in their party, with the session's players, its join code and travel a press away; alone, the map
// and the time, with travel and ONLINE.
void where_you_are(SkateMenu& menu, const Model& model) {
    static constexpr std::array<const char*, 8> times{"", "morning", "noon", "afternoon", "evening", "night", "weather day", "weather night"};
    const auto& mp = model.multiplayer;
    const bool session = mp.active && !mp.map_fetching;
    const auto level = loaded_level_name(model);
    const char* time = times[static_cast<std::size_t>(std::clamp(time_of_day_shown(model), 0, 7))];
    std::string title, line;
    const auto add = [&](const std::string& part) {
        if (!part.empty()) line += (line.empty() ? "" : " \xc2\xb7 ") + part;
    };
    if (session) {
        title = !mp.lobby_name.empty() ? mp.lobby_name : mp.hosting ? std::string("Your session") : std::string("A session");
        add(level);
        add(time);
        add(std::to_string(mp.players) + " of " + std::to_string(mp.capacity) + " skaters");
        if (mp.party) {
            const auto members = 1 + std::ranges::count_if(mp.roster, [](const MultiplayerPlayer& p) { return p.party_member; });
            add(std::to_string(members) + " in your party");
        }
    } else {
        title = level.empty() ? std::string("skate.") : level;
        add(time);
        add(model.steam_offline ? "offline" : "on your own");
    }
    const std::string code = !mp.join_code.empty() ? mp.join_code : mp.invite;
    const auto at = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x, height = px(146);
    auto* draw = ImGui::GetWindowDrawList();
    tile_rect(draw, at, ImVec2(at.x + width, at.y + height), blue, 0x5a1, px(1));
    draw->PushClipRect(at, ImVec2(at.x + width - px(16), at.y + height), true);
    draw->AddText(menu.bold, px(14), ImVec2(at.x + px(18), at.y + px(16)), IM_COL32(219, 238, 255, 255), "YOU ARE ON");
    draw->AddText(menu.heading, px(22), ImVec2(at.x + px(18), at.y + px(36)), paper, title.c_str());
    draw->AddText(menu.body, px(15), ImVec2(at.x + px(18), at.y + px(64)), IM_COL32(227, 241, 255, 255), line.c_str());
    draw->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(at.x + px(18), at.y + px(96)));
    const float chip = std::min(px(130), (width - px(36) - px(12)) / 3.0f);
    if (session) {
        if (where_chip(menu, "PLAYERS", chip, "Online's session: who is here, and what you can do with them."))
            open_page(menu, page_online, multiplayer_session_tab);
        if (!code.empty()) {
            ImGui::SameLine(0, px(6));
            if (where_chip(menu, "INVITE", chip, "Copies the session's join code. Friends paste it in Servers > Join with a code.")) {
                ImGui::SetClipboardText(code.c_str());
                feedback(menu, "Join code copied.");
            }
        }
        ImGui::SameLine(0, px(6));
        if (where_chip(menu, "TRAVEL", chip, "Map > Travel.")) open_page(menu, page_map, 0);
    } else {
        if (where_chip(menu, "TRAVEL", chip, "Map > Travel: pick a map and load it.")) open_page(menu, page_map, 0);
        if (!model.steam_offline) {
            ImGui::SameLine(0, px(6));
            if (where_chip(menu, "ONLINE", chip, "Join a server or a friend's lobby, or host your own.")) open_page(menu, page_online);
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + height));
    ImGui::Dummy(ImVec2(0, px(10)));
}
}

void home_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    ImGui::BeginChild("home", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    where_you_are(menu, model);

    // ---- the switches people reach for while skating: three to a row
    heading(menu, "QUICK");
    {
        const float gap = px(6), room = ImGui::GetContentRegionAvail().x, left = ImGui::GetCursorPosX();
        const float width = (room - gap * 2) / 3;
        int column = 0;
        const auto cell = [&] {
            if (column == 3) {
                ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
                column = 0;
            }
            if (column > 0) ImGui::SameLine(left + (width + gap) * static_cast<float>(column));
            ++column;
            return ImVec2(width, px(78));
        };
        const auto& debug = model.debug;
        const auto on_off = [](bool value) { return std::string(value ? "ON" : "OFF"); };
        const auto request = [&](DebugAction action, bool value) {
            if (!callbacks.queue_debug) return;
            std::array<char, 256> reply{};
            callbacks.queue_debug(callbacks.user, {action, value}, reply.data(), reply.size());
        };
        if (home_tile(menu, "##noclip", "NOCLIP", on_off(debug.noclip), cell(), (debug.noclip_available || debug.noclip) && callbacks.queue_debug,
                      debug.noclip, false, "Fly with the normal player camera, through anything. Includes No Bail.") == 1)
            request(DebugAction::set_noclip, !debug.noclip);
        if (home_tile(menu, "##no-bail", "NO BAIL", on_off(debug.no_bail), cell(), (debug.no_bail_available || debug.no_bail) && callbacks.queue_debug,
                      debug.no_bail, false, "Never wipe out.") == 1)
            request(DebugAction::set_no_bail, !debug.no_bail);
        if (home_tile(menu, "##freecam", "FREECAM", on_off(debug.free_camera), cell(), debug.camera_available && callbacks.queue_debug,
                      debug.free_camera, false, "Leave your skater and fly the camera.") == 1)
            request(DebugAction::set_free_camera, !debug.free_camera);
        if (home_tile(menu, "##first-person", "FIRST PERSON", on_off(debug.first_person), cell(), debug.available && callbacks.queue_debug,
                      debug.first_person, false, "See it through your skater's eyes.") == 1)
            request(DebugAction::set_first_person, !debug.first_person);
        if (home_tile(menu, "##hide-hud", "HIDE HUD", on_off(debug.game_ui_hidden), cell(), debug.available && debug.ui_available && callbacks.queue_debug,
                      debug.game_ui_hidden, false, "Keep the view clear for riding and captures.") == 1)
            request(DebugAction::set_game_ui_hidden, !debug.game_ui_hidden);
        namespace tod = dingosdk::native_tools;
        static constexpr std::array<const char*, 8> times{"Default", "Morning", "Noon", "Afternoon", "Evening", "Night", "Weather day", "Weather night"};
        const bool tod_available = tod::time_of_day_available(model, callbacks);
        const int now = time_of_day_shown(model);
        const int step = home_tile(menu, "##time", "TIME", times[static_cast<std::size_t>(std::clamp(now, 0, 7))], cell(), tod_available, now != 0,
                                   true, "Press A to change the time of day. Default follows the level's own.");
        // One time a press, on from the one last asked for.
        if (step) set_time_of_day(menu, model, callbacks, (now + step + 8) % 8, true);
    }
    ImGui::Dummy(ImVec2(0, px(14)));

    // ---- the pages opened lately, and what wants a look
    heading(menu, "PICK UP WHERE YOU LEFT OFF");
    {
        const auto go = [&](const SkateMenu::RecentPage& recent) {
            open_page(menu, recent.page, recent.tab);
            if (recent.page == page_plugins) menu.plugin_page = recent.plugin.empty() ? std::string(plugin_manager_entry) : recent.plugin;
        };
        int rows = 0;
        // (Copied: going to one reorders the list.)
        const auto recent = menu.recent;
        for (std::size_t i = 0; i < recent.size(); ++i) {
            const auto& entry = recent[i];
            if (entry.page == page_online && model.steam_offline) continue;
            LibraryRow row;
            row.name = entry.label;
            row.side = entry.plugin.empty() ? "Last opened \xe2\x80\xba" : "Plugin page \xe2\x80\xba";
            row.hint = "A opens it again.";
            ImGui::PushID(static_cast<int>(i));
            if (library_row(menu, "##recent", row) == RowPress::a) go(entry);
            ImGui::PopID();
            ++rows;
        }
        const auto mods = mod_attention();
        if (mods.updates || mods.problems) {
            LibraryRow row;
            row.name = "Mods";
            if (mods.updates) row.side = std::to_string(mods.updates) + (mods.updates == 1 ? " needs an update" : " need an update");
            if (mods.problems)
                row.side += (row.side.empty() ? "" : ", ") + std::to_string(mods.problems) + (mods.problems == 1 ? " problem" : " problems");
            row.side += " \xe2\x80\xba";
            row.side_colour = skate_theme::warning;
            row.hint = "A opens MODS.";
            if (library_row(menu, "##mods-attention", row) == RowPress::a) open_page(menu, page_mods);
            ++rows;
        }
        const auto plugins = plugins::installed();
        const auto asking = std::ranges::count_if(plugins, [](const plugins::Installed& p) { return p.approval == plugins::Approval::asking; });
        if (asking) {
            LibraryRow row;
            row.name = "Plugins";
            row.side = std::to_string(asking) + (asking == 1 ? " asks for your OK \xe2\x80\xba" : " ask for your OK \xe2\x80\xba");
            row.side_colour = skate_theme::warning;
            row.hint = "A opens PLUGINS, to answer.";
            if (library_row(menu, "##plugins-attention", row) == RowPress::a) {
                open_page(menu, page_plugins);
                menu.plugin_page = plugin_manager_entry;
            }
            ++rows;
        }
        if (!rows) note("The pages you open show up here, to go back to them in one press. LB / RB go through every page.");
    }
    ImGui::EndChild();
}
}
