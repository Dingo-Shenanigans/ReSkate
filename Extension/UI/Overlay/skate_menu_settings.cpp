#include "Engine/Core/Platform/launcher_support.h"
#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Music/local_music_playback.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Plugins/plugin_ui.h"

#include <algorithm>
#include <array>

// The SETTINGS and DEVELOPER pages.
namespace dingosdk::overlay::menu {
using namespace theme;
void music_playback_card(SkateMenu& menu) {
    begin_card(menu, "music-playback", "MUSIC PLAYBACK");
    bool shuffle = dingosdk::profile_runtime::music_shuffle_enabled();
    if (toggle_row(menu, "Shuffle playlists", "Off plays songs in playlist order. On shuffles.", shuffle,
            dingosdk::profile_runtime::music_playback_available())) {
        dingosdk::profile_runtime::set_music_shuffle_enabled(shuffle);
        dingosdk::profile_runtime::set_local_preference("MusicShuffle", shuffle);
    }
    end_card();
}

// Settings > SCREEN: what shows, and how this menu looks.
void screen_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "on-screen", "ON SCREEN");
    bool hidden = model.debug.game_ui_hidden;
    if (toggle_row(menu, "Hide game UI", "Keep the view clear for riding and captures.", hidden,
            model.debug.available && model.debug.ui_available && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_game_ui_hidden, hidden});
    const auto back = dingosdk::launcher::key_name(dingosdk::launcher::overlay_keys().menu) + " always brings ReSkate back.";
    note(back.c_str());
    end_card();

    begin_card(menu, "menu-layout", "MENU LAYOUT");
    int layout = menu_layout(menu, model);
    if (choice(menu, "menu-layout", layout, {"SIDE SHEET", "WINDOW"}, callbacks.queue_console_command != nullptr, 0.0f,
            {"Docked to the right edge, the game in view beside it. Drag its left edge to make it wider.",
             "A window in the middle of the screen, the pages down its left side. Drag an edge or a corner to resize it."}))
        set_menu_layout(menu, callbacks, layout);
    note("Also F11, or View on a controller. Saved with your profile, with the size you drag either to.");
    end_card();

    // One size for both layouts.
    begin_card(menu, "menu-scale", "MENU SIZE");
    // Step locally for an immediate preview, then save once let go: the
    // saved value only returns through the model on a later frame.
    if (!menu.scale_editing) menu.scale = model.menu_scale;
    field(menu, "Menu size", "Left / right to change it.");
    ::dingosdk::overlay::menu::slider_float("##menu-scale", &menu.scale, min_menu_scale, max_menu_scale, "%.2fx",
        ImGuiSliderFlags_AlwaysClamp);
    menu.scale_editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        send_console(menu, callbacks, "ui scale " + std::to_string(menu.scale));
    if (button_row(menu, "scale-reset", {{"RESET SIZE", menu.scale != default_menu_scale}}) == 0) {
        menu.scale = default_menu_scale;
        send_console(menu, callbacks, "ui scale " + std::to_string(default_menu_scale));
    }
    note("Scales the menu and its text in both layouts. It grows with the screen too, so a size reads the same at 1080p and 4K. "
         "Saved with your profile.");
    end_card();
    graphics_page(menu, model, callbacks);
}

// Settings > PERFORMANCE.
void performance_page(SkateMenu& menu, const Model&, const CallbacksV3& callbacks) {
    begin_card(menu, "performance", "PERFORMANCE");
    // The profiler is thread-safe: its state is read here directly, not through the model.
    bool hud = dingosdk::profiler::hud();
    if (toggle_row(menu, "Performance HUD", "Client update and frame timing, ReSkate's cost and CPU per thread.",
            hud, callbacks.queue_console_command != nullptr))
        send_console(menu, callbacks, hud ? "perf hud on" : "perf hud off");
    bool window = dingosdk::profiler::window();
    if (toggle_row(menu, "Profiler window", "Zones, threads and a stack sampler. Shows while this menu or the console is open.",
            window))
        dingosdk::profiler::set_window(window);
    note("Console: perf, perf sample [seconds] [client|present|thread id], perf report, perf status.");
    end_card();
}

namespace {
// The binds in the groups they are looked for in. An action is its number in `bind` (1..9), or
// 10 + its place in action_binds.
struct BindGroup {
    const char* title;
    std::initializer_list<int> actions;
};
constexpr int action_bind(std::string_view key) {
    for (std::size_t i = 0; i < action_binds.size(); ++i)
        if (action_binds[i].key == key) return 10 + static_cast<int>(i);
    return 0;
}
const std::array<BindGroup, 4> bind_groups{{
    {"GENERAL", {action_bind("reskate_menu"), action_bind("hide_hud"), action_bind("challenges"), action_bind("time_of_day")}},
    {"CAMERA", {2, 1, 6, action_bind("first_person")}},
    {"CHEATS AND BOOSTS", {3, action_bind("no_bail"), 4, 5, 7}},
    {"ONLINE", {action_bind("voice_chat"), action_bind("nametags"), action_bind("nametag_dots"), 8, 9}},
}};
const char* bind_name(int action) {
    switch (action) {
    case 1: return "Freecam Controller";
    case 2: return "Freecam";
    case 3: return "Noclip";
    case 4: return "Forward Boost";
    case 5: return "Up Boost";
    case 6: return "TP to Freecam";
    case 7: return "Off-board Up Boost";
    case 8: return "Vote yes";
    case 9: return "Vote no";
    default: return action >= 10 ? action_binds[static_cast<std::size_t>(action - 10)].label.data() : "";
    }
}
std::uint32_t bind_combo(const ControllerBindingsModel& b, int action) {
    switch (action) {
    case 1: return b.freecam_controller_combo;
    case 2: return b.freecam_combo;
    case 3: return b.noclip_combo;
    case 4: return b.forward_velocity_combo;
    case 5: return b.up_velocity_combo;
    case 6: return b.tp_to_freecam_combo;
    case 7: return b.offboard_up_velocity_combo;
    case 8: return b.vote_yes_combo;
    case 9: return b.vote_no_combo;
    default: return action >= 10 ? b.action_combos[static_cast<std::size_t>(action - 10)] : 0;
    }
}
}

void binds_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    const bool available = model.bindings.available && callbacks.queue_console_command;
    // Plugins' binds (reskate.input.bind) are rows from plugin_bind_row up, in the order the
    // plugins' list gives them; the player's combo replaces the plugin's own.
    constexpr int plugin_bind_row = 1000;
    const auto plugin_binds = plugin_ui::latest_binds();
    const auto save = [&](int action, std::uint32_t combo) {
        if (action >= plugin_bind_row) {
            const auto index = static_cast<std::size_t>(action - plugin_bind_row);
            if (index < plugin_binds->size())
                plugin_ui::rebind({(*plugin_binds)[index].plugin, (*plugin_binds)[index].action, combo});
            feedback(menu, combo ? "Saved." : "Cleared. The plugin's action has no bind now.");
            return;
        }
        std::array<char, 512> result{};
        const auto command = std::string("bind ") +
            (action >= 10 ? std::string(action_binds[static_cast<std::size_t>(action - 10)].name) + " " : action == 1 ? "freecamcontroller " : action == 2 ? "freecam " : action == 3 ? "noclip " : action == 4 ? "forwardvelocity " : action == 5 ? "upvelocity " : action == 6 ? "tptofreecam " : action == 8 ? "voteyes " : action == 9 ? "voteno " : "offboardupvelocity ") + std::to_string(combo);
        const bool queued = callbacks.queue_console_command(callbacks.user, command.c_str(), result.data(), result.size());
        result.back() = '\0';
        feedback(menu, result[0] ? result.data() : queued ? "Saving binding..." : "Could not queue binding.");
    };
    ControllerInput controller;
    DingoSDKOverlayReadControllerInput(&controller, true);
    if (menu.recording_bind) {
        if (!available || ImGui::GetTime() >= menu.bind_capture_until) {
            menu.recording_bind = 0;
            feedback(menu, "Recording cancelled. Your binding is unchanged.");
        } else if (const auto combo = menu.bind_capture.update(controller, true)) {
            const auto action = menu.recording_bind;
            menu.recording_bind = 0;
            save(action, *combo);
        }
    }
    const auto record = [&](int action) {
        menu.bind_capture = {};
        menu.recording_bind = action;
        menu.bind_capture_until = ImGui::GetTime() + 10;
        menu.feedback.clear();
    };
    if (menu.recording_bind) {
        warn(!menu.bind_capture.ready ? "Release controller buttons and keyboard keys first."
                                      : "Hold up to five buttons or keys together, then let go. Nothing for 10 seconds cancels.");
    }
    struct Row {
        int action;
        std::string name;
        std::uint32_t combo;
    };
    struct Card {
        const char* title;
        std::vector<Row> rows;
    };
    std::vector<Card> cards;
    for (const auto& group : bind_groups) {
        if (std::string_view(group.title) == "ONLINE" && model.steam_offline) continue;
        auto& card = cards.emplace_back(Card{group.title, {}});
        for (const int action : group.actions) card.rows.push_back({action, bind_name(action), bind_combo(model.bindings, action)});
    }
    if (!plugin_binds->empty()) {
        auto& card = cards.emplace_back(Card{"PLUGINS", {}});
        for (std::size_t i = 0; i < plugin_binds->size(); ++i) {
            const auto& bind = (*plugin_binds)[i];
            card.rows.push_back({plugin_bind_row + static_cast<int>(i), bind.plugin + ": " + bind.action, bind.combo});
        }
    }
    for (const auto& group : cards) {
        begin_card(menu, group.title, group.title);
        ImGui::PushID(group.title);
        // A row a bind, its combo at the right end. A records, Y clears; the row being recorded is blue
        // while the pad belongs to the recording (menu_page_owns_pad), and the others wait.
        for (const auto& row : group.rows) {
            const int action = row.action;
            const std::uint32_t combo = row.combo;
            const bool recording = menu.recording_bind == action;
            const auto side = recording ? std::string(menu.bind_capture.ready ? "Press the buttons..." : "Let go first...")
                                        : controller_combo_label(combo, controller.style);
            ImGui::PushID(action);
            const bool pressed = list_row(menu, "##bind", row.name, side, recording, available && (!menu.recording_bind || recording));
            const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible;
            if (pressed && !menu.recording_bind) record(action);
            if (focused && !menu.recording_bind && combo && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) save(action, 0);
            if (focused || ImGui::IsItemHovered())
                set_console_hint(recording ? "Hold the buttons together, then let go."
                                           : combo ? "A records a new bind. Y clears this one." : "A records a bind.");
            ImGui::PopID();
        }
        if (std::string_view(group.title) == "PLUGINS") {
            const bool changed = std::any_of(plugin_binds->begin(), plugin_binds->end(), [](const auto& b) { return b.combo != b.own; });
            if (button_row(menu, "plugin-binds-reset", {{"PLUGINS' OWN BINDS", changed && menu.recording_bind == 0, false,
                                                         "Every plugin's action back on the bind the plugin gave it."}}) == 0) {
                for (const auto& bind : *plugin_binds) plugin_ui::rebind({bind.plugin, bind.action, std::nullopt});
                feedback(menu, "Every plugin's action is on the plugin's own bind again.");
            }
        }
        ImGui::PopID();
        end_card();
    }
    if (!menu.recording_bind) {
        note(("A key, a chord such as Ctrl + F5, or a controller combo such as " + controller_combo_label(0x300, controller.style) +
              ". Freecam, Freecam Controller and Noclip toggle: give them different combos. Boosts add speed once a press; "
              "Off-board Up Boost also works falling or gliding, and on- and off-board boosts can share a combo.").c_str());
        note("Saved to your profile.");
        if (!model.bindings.status.empty()) note(model.bindings.status.c_str());
        if (!model.bindings.available) warn("Waiting for the local profile.");
    }
    if (!model.steam_offline) note("The push-to-talk button is set in Online > Voice.");
}

void settings_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    category_tabs(menu, menu.settings_tab, {"CONTROLS", "SCREEN", "PERFORMANCE"}, "settings-tabs");
    ImGui::PushID(menu.settings_tab);
    ImGui::BeginChild("settings-tab", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    switch (menu.settings_tab) {
    case 0: binds_page(menu, model, callbacks); break;
    case 1: screen_page(menu, model, callbacks); break;
    case 2: performance_page(menu, model, callbacks); break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void developer_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    ImGui::BeginChild("developer", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    multiplayer_network_page(menu, model, callbacks);
    ImGui::EndChild();
}
}
