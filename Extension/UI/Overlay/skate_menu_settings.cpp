#include "Engine/Core/Platform/launcher_support.h"
#include "skate_menu_internal.h"
#include "Engine/Core/Profiling/profiler.h"
#include "Extension/Music/local_music_playback.h"
#include "Extension/Profile/local_profile_runtime.h"

#include <algorithm>
#include <array>

// The SETTINGS and DEVELOPER pages.
namespace dingosdk::overlay::menu {
void ui_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "music-playback", "MUSIC PLAYBACK");
    bool shuffle = dingosdk::profile_runtime::music_shuffle_enabled();
    if (toggle_row(menu, "Shuffle playlists", "Off plays songs in playlist order. On shuffles.", shuffle,
            dingosdk::profile_runtime::music_playback_available())) {
        dingosdk::profile_runtime::set_music_shuffle_enabled(shuffle);
        dingosdk::profile_runtime::set_local_preference("MusicShuffle", shuffle);
    }
    end_card();

    begin_card(menu, "on-screen", "ON SCREEN");
    bool hidden = model.debug.game_ui_hidden;
    if (toggle_row(menu, "Hide game UI", "Keep the view clear for riding and captures.", hidden,
            model.debug.available && model.debug.ui_available && callbacks.queue_debug))
        debug_request(menu, callbacks, {DebugAction::set_game_ui_hidden, hidden});
    const auto back = dingosdk::launcher::key_name(dingosdk::launcher::overlay_keys().menu) + " always brings ReSkate back.";
    note(back.c_str());
    end_card();

    begin_card(menu, "menu-scale", "MENU SIZE");
    // Drag locally for an immediate preview, then save once on release: the
    // saved value only returns through the model on a later frame.
    if (!menu.scale_editing) menu.scale = model.menu_scale;
    field(menu, "ReSkate menu size");
    const bool resettable = menu.scale != default_menu_scale;
    const float reset = ImGui::CalcTextSize("Reset").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - reset - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SliderFloat("##menu-scale", &menu.scale, min_menu_scale, max_menu_scale, "%.2fx",
        ImGuiSliderFlags_AlwaysClamp);
    menu.scale_editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        send_console(menu, callbacks, "ui scale " + std::to_string(menu.scale));
    ImGui::SameLine();
    ImGui::BeginDisabled(!resettable);
    if (ImGui::Button("Reset")) {
        menu.scale = default_menu_scale;
        send_console(menu, callbacks, "ui scale " + std::to_string(default_menu_scale));
    }
    ImGui::EndDisabled();
    note("Scales this menu and its text. Saved with your profile.");
    end_card();

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
    if (!model.steam_offline) multiplayer_display_settings(menu, model);
}

void binds_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    begin_card(menu, "controller-binds", "ACTION BINDS");
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
        } else if (const auto combo = menu.bind_capture.update(controller, true)) {
            const auto action = menu.recording_bind;
            menu.recording_bind = 0;
            save(action, *combo);
        }
    }
    ImGui::BeginDisabled(!available);
    if (ImGui::BeginTable("controller-binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, px(130));
        ImGui::TableSetupColumn("Controller combo / key", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##controls", ImGuiTableColumnFlags_WidthFixed, px(170));
        ImGui::TableHeadersRow();
        const auto row = [&](int action, const char* name, std::uint32_t combo) {
            ImGui::PushID(action);
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(name);
            ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
            const auto label = menu.recording_bind == action ? "Recording..." : controller_combo_label(combo, controller.style);
            ImGui::TextWrapped("%s", label.c_str());
            ImGui::TableNextColumn();
            if (menu.recording_bind == action) {
                if (ImGui::Button("Cancel", ImVec2(-1, 0))) menu.recording_bind = 0;
            } else {
                ImGui::BeginDisabled(menu.recording_bind != 0);
                if (ImGui::Button("Record", ImVec2(px(90), 0))) {
                    menu.bind_capture = {};
                    menu.recording_bind = action;
                    menu.bind_capture_until = ImGui::GetTime() + 30;
                    menu.feedback.clear();
                }
                ImGui::SameLine(); ImGui::BeginDisabled(!combo);
                if (ImGui::Button("Clear", ImVec2(-1, 0))) save(action, 0);
                ImGui::EndDisabled();
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        };
        row(1, "Freecam Controller", model.bindings.freecam_controller_combo);
        row(2, "Freecam", model.bindings.freecam_combo);
        row(3, "Noclip", model.bindings.noclip_combo);
        row(4, "Forward Boost", model.bindings.forward_velocity_combo);
        row(5, "Up Boost", model.bindings.up_velocity_combo);
        row(6, "TP to Freecam", model.bindings.tp_to_freecam_combo);
        row(7, "Off-board Up Boost", model.bindings.offboard_up_velocity_combo);
        row(8, "Vote yes", model.bindings.vote_yes_combo);
        row(9, "Vote no", model.bindings.vote_no_combo);
        for (std::size_t i = 0; i < action_binds.size(); ++i)
            row(10 + static_cast<int>(i), action_binds[i].label.data(), model.bindings.action_combos[i]);
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    if (menu.recording_bind) {
        warn(!menu.bind_capture.ready ? "Release controller buttons and keyboard keys first." :
             "Hold up to five keys together, then release them. A-Z, 0-9, Space, F1-F12, Ctrl, Shift and Alt work; controller combos work too.");
    } else {
        note(("Record a key, keyboard chord or controller combo, such as Ctrl + F5 or " + controller_combo_label(0x300, controller.style) +
              ". Freecam, Freecam Controller, and Noclip toggle; boosts add velocity once per press. Off-board Up Boost also works while falling or gliding; release and press again to repeat. On-board and off-board boosts can share a combo. Give toggles different combos.").c_str());
        note("Saved to your profile.");
        if (!model.bindings.status.empty()) note(model.bindings.status.c_str());
        if (!model.bindings.available) warn("Waiting for the local profile.");
    }
    if (!model.steam_offline) note("The push-to-talk button is set in Multiplayer > Voice.");
    end_card();
}

void settings_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    menu.settings_tab = std::min(menu.settings_tab, 2); // Mods moved to its own page
    category_tabs(menu, menu.settings_tab, {"CONTROLS", "INTERFACE", "POST FX"}, "settings-tabs");
    ImGui::PushID(menu.settings_tab);
    ImGui::BeginChild("settings-tab", ImVec2(0, page_body_height(menu)));
    switch (menu.settings_tab) {
    case 0: binds_page(menu, model, callbacks); break;
    case 1: ui_page(menu, model, callbacks); break;
    case 2: graphics_page(menu, model, callbacks); break;
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void developer_page(SkateMenu& menu, const Model& model, const CallbacksV3& callbacks) {
    ImGui::BeginChild("developer", ImVec2(0, page_body_height(menu)));
    multiplayer_network_page(menu, model, callbacks);
    ImGui::EndChild();
}
}
