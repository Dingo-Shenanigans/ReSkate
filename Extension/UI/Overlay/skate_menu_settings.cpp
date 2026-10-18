#include "Engine/Core/Platform/launcher_support.h"
#include "skate_menu_internal.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Music/local_music_playback.h"
#include "Extension/Profile/local_profile_runtime.h"

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

    // The size of the style this is drawn in: the window and full screen keep their own.
    const bool full = console_style();
    const char* command = full ? "ui scalefull " : "ui scale ";
    begin_card(menu, "menu-scale", "MENU SIZE", full ? "Full screen" : "Window");
    // Drag locally for an immediate preview, then save once on release: the
    // saved value only returns through the model on a later frame.
    if (!menu.scale_editing) menu.scale = full ? model.menu_scale_fullscreen : model.menu_scale;
    field(menu, full ? "Full screen menu size" : "Window menu size");
    const bool resettable = menu.scale != default_menu_scale;
    const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reset - ImGui::GetStyle().ItemSpacing.x);
    ::dingosdk::overlay::menu::slider_float("##menu-scale", &menu.scale, min_menu_scale, max_menu_scale, "%.2fx",
        ImGuiSliderFlags_AlwaysClamp);
    menu.scale_editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        send_console(menu, callbacks, command + std::to_string(menu.scale));
    if (!full) ImGui::SameLine();
    if (button_row(menu, "scale-reset", {{full ? "RESET SIZE" : "Reset", resettable}}) == 0) {
        menu.scale = default_menu_scale;
        send_console(menu, callbacks, command + std::to_string(default_menu_scale));
    }
    note(full ? "Scales the full screen menu and its text; the window keeps its own size. Saved with your profile."
              : "Scales this window and its text; the full screen menu keeps its own size. Saved with your profile.");
    end_card();

    begin_card(menu, "menu-style", "MENU STYLE");
    int style = menu_fullscreen(menu, model) ? 1 : 0;
    if (choice(menu, "menu-style", style, {"Window", "Full screen"}, callbacks.queue_console_command != nullptr, 0.0f,
            {"A window over the game you can move and resize: made for a mouse.",
             "Fills the screen, with bigger rows and button prompts: made for a controller."}))
        set_menu_fullscreen(menu, callbacks, style == 1);
    note("Also the button beside the menu's close button, F11, or View on a controller. Saved with your profile.");
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
    {"CAMERA", {2, 1, 6, action_bind("first_person"), action_bind("first_person_glance_up"), action_bind("first_person_glance_down"), action_bind("first_person_glance_left"), action_bind("first_person_glance_right"), action_bind("first_person_modifier")}},
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
    const auto save = [&](int action, std::uint32_t combo) {
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
        } else if (const auto combo = menu.bind_capture.update(controller, menu.recording_bind < 10 || !action_binds[static_cast<std::size_t>(menu.recording_bind-10)].controller_only)) {
            const auto action = menu.recording_bind;
            menu.recording_bind = 0;
            save(action, *combo);
        }
    }
    const auto record = [&](int action) {
        menu.bind_capture = {};
        menu.recording_bind = action;
        menu.bind_capture_until = ImGui::GetTime() + (console_style() ? 10 : 30);
        menu.feedback.clear();
    };
    const bool console = console_style();
    if (menu.recording_bind) {
        warn(!menu.bind_capture.ready ? "Release controller buttons and keyboard keys first." :
             (menu.recording_bind>=10 && action_binds[static_cast<std::size_t>(menu.recording_bind-10)].controller_only) ? "Hold the controller modifier combo, then release it. This action uses controller buttons." :
             console ? "Hold up to five buttons together, then let go. Nothing for 10 seconds cancels." :
             "Hold up to five keys together, then release them. A-Z, 0-9, Space, F1-F12, Ctrl, Shift and Alt work; controller combos work too.");
    }
    for (const auto& group : bind_groups) {
        if (std::string_view(group.title) == "ONLINE" && model.steam_offline) continue;
        begin_card(menu, group.title, group.title);
        ImGui::PushID(group.title);
        if (console) {
            // Full screen: a row a bind, its combo at the right end. A records, Y clears.
            auto* draw = ImGui::GetWindowDrawList();
            for (const int action : group.actions) {
                const std::uint32_t combo = bind_combo(model.bindings, action);
                const bool recording = menu.recording_bind == action;
                ImGui::PushID(action);
                const auto at = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x, height = px(54);
                ImGui::BeginDisabled(!available || (menu.recording_bind && !recording));
                const bool pressed = ImGui::InvisibleButton("##bind", ImVec2(width, height), ImGuiButtonFlags_EnableNav);
                ImGui::EndDisabled();
                const bool focused = ImGui::IsItemFocused() && ImGui::GetIO().NavVisible, hovered = ImGui::IsItemHovered();
                if (pressed && !menu.recording_bind) record(action);
                if (focused && !menu.recording_bind && combo && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false)) save(action, 0);
                if (focused || hovered)
                    set_console_hint(recording ? "Hold the buttons together, then let go."
                                               : combo ? "A records a new bind. Y clears this one." : "A records a bind.");
                const ImVec2 end(at.x + width, at.y + height);
                const bool lit = focused || hovered;
                skate_theme::rough_rect(draw, at, end, recording ? paper : lit ? blue : IM_COL32(26, 26, 26, 235),
                                        static_cast<unsigned>(ImGui::GetItemID()) & 0xfff, px(1));
                const ImU32 text = recording ? skate_theme::black : available ? paper : muted;
                const auto name = menu.bold->CalcTextSizeA(px(17), FLT_MAX, 0, bind_name(action));
                draw->AddText(menu.bold, px(17), ImVec2(at.x + px(18), at.y + (height - name.y) * .5f), text, bind_name(action));
                const auto label = recording ? std::string(menu.bind_capture.ready ? "Press the buttons..." : "Let go first...")
                                             : controller_combo_label(combo, controller.style);
                const auto size = menu.bold->CalcTextSizeA(px(17), FLT_MAX, 0, label.c_str());
                draw->AddText(menu.bold, px(17), ImVec2(end.x - px(24) - size.x, at.y + (height - size.y) * .5f),
                              recording ? skate_theme::black : lit ? paper : combo ? blue : muted, label.c_str());
                ImGui::PopID();
            }
        } else {
            ImGui::BeginDisabled(!available);
            if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(150));
                ImGui::TableSetupColumn("Controller combo / key", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("##controls", ImGuiTableColumnFlags_WidthFixed, px(170));
                for (const int action : group.actions) {
                    const std::uint32_t combo = bind_combo(model.bindings, action);
                    ImGui::PushID(action);
                    ImGui::TableNextRow(); ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(bind_name(action));
                    ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                    const auto label = menu.recording_bind == action ? "Recording..." : controller_combo_label(combo, controller.style);
                    ImGui::TextWrapped("%s", label.c_str());
                    ImGui::TableNextColumn();
                    if (menu.recording_bind == action) {
                        if (ImGui::Button("Cancel", ImVec2(-1, 0))) menu.recording_bind = 0;
                    } else {
                        ImGui::BeginDisabled(menu.recording_bind != 0);
                        if (ImGui::Button("Record", ImVec2(px(90), 0))) record(action);
                        ImGui::SameLine(); ImGui::BeginDisabled(!combo);
                        if (ImGui::Button("Clear", ImVec2(-1, 0))) save(action, 0);
                        ImGui::EndDisabled();
                        ImGui::EndDisabled();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
        }
        ImGui::PopID();
        end_card();
    }
    if (!menu.recording_bind) {
        note(("Record a key, keyboard chord or controller combo, such as Ctrl + F5 or " + controller_combo_label(0x300, controller.style) +
              ". Freecam, Freecam Controller, and Noclip toggle; boosts add velocity once per press. Off-board Up Boost also works while falling or gliding; release and press again to repeat. On-board and off-board boosts can share a combo. Give toggles different combos.").c_str());
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
