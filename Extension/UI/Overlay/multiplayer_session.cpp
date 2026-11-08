#include "multiplayer_menu_internal.h"
#include "input_capture.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Game/Input/voice_input.h"
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <utility>
#include <Windows.h>
#include <shellapi.h>

namespace dingosdk::overlay::menu::multiplayer_detail {
namespace {
// Who may change the session's settings: its host, or an admin of the dedicated
// server it runs on (their changes are sent to the server).
bool controls_session(const MultiplayerModel &mp) { return mp.hosting || mp.server_admin; }
// The same for one setting: the host, or a role with the server command that changes it.
bool controls(const MultiplayerModel &mp, std::string_view command) { return mp.hosting || server_can(mp, command); }
const char *set_by(const MultiplayerModel &mp, std::string_view command) {
    return controls(mp, command) ? nullptr : mp.dedicated ? "Set by the server" : "Set by the host";
}
const char *set_by(const MultiplayerModel &mp) {
    return controls_session(mp) ? nullptr : mp.dedicated ? "Set by the server" : "Set by the host";
}
// A player's voice state as the model has it, or the defaults for one it has not heard yet.
VoicePlayer voice_of(const MultiplayerModel &mp, std::uint64_t id) {
    const auto found = std::find_if(mp.voice.players.begin(), mp.voice.players.end(), [&](const auto &p) { return p.id == id; });
    return found == mp.voice.players.end() ? VoicePlayer{id} : *found;
}
// How loud this PC hears one player: on the VOICE tab's rows and the SESSION tab's. The slider shows
// what was just set until the model agrees (or two seconds pass).
void player_volume(SkateMenu &menu, const MultiplayerModel &mp, const MultiplayerPlayer &peer) {
    std::array<char, 65> unused{};
    std::erase_if(menu.voice_volume_pending, [&](const auto &item) { return ImGui::GetTime() >= item.second.second; });
    auto gain = voice_of(mp, peer.id).volume;
    if (const auto pending = menu.voice_volume_pending.find(peer.id); pending != menu.voice_volume_pending.end()) {
        if (pending->second.first == gain) menu.voice_volume_pending.erase(pending);
        else gain = pending->second.first;
    }
    field(menu, "Volume", "How loud you hear this player. Only you hear the change.");
    if (::dingosdk::overlay::menu::slider_float("##volume", &gain, 0.f, max_voice_volume, "%.2fx", ImGuiSliderFlags_AlwaysClamp) &&
        send_private(menu, "voice-volume", std::to_string(peer.id) + " " + std::to_string(gain), unused, false))
        menu.voice_volume_pending[peer.id] = {gain, ImGui::GetTime() + 2};
}
// The day a ban was added, or "-" for one without a date.
std::string ban_date(std::int64_t added) {
    std::array<char, 32> date{};
    const auto time = static_cast<std::time_t>(added);
    std::tm local{};
    if (added > 0 && localtime_s(&local, &time) == 0) std::strftime(date.data(), date.size(), "%d %b %Y", &local);
    return date[0] ? date.data() : "-";
}
void distance_settings(SkateMenu &menu, const MultiplayerModel &mp) {
    if (!menu.multiplayer_distance_initialized || menu.multiplayer_distance_applied != mp.distances ||
        menu.multiplayer_distance_hosting != mp.hosting || menu.multiplayer_distance_lobby != mp.invite) {
        menu.multiplayer_distance_draft = menu.multiplayer_distance_applied = mp.distances;
        menu.multiplayer_distance_hosting = mp.hosting;
        menu.multiplayer_distance_lobby = mp.invite;
        menu.multiplayer_distance_initialized = true;
    }
    auto &draft = menu.multiplayer_distance_draft;
    const bool editable = controls(mp, "distances");
    begin_card(menu, "distances", "NETWORK DISTANCES", set_by(mp, "distances"));
    note("Skaters further away update less often, which keeps big lobbies smooth.");
    ImGui::BeginDisabled(!editable);
    // Left / right move a distance 5 m at a time; the box beside it takes an exact one.
    const auto distance = [&](const char *label, const char *id, int &value, int low, int high) {
        field(menu, label);
        set_console_step(5);
        ::dingosdk::overlay::menu::slider_int(id, &value, low, high, "%d m", ImGuiSliderFlags_AlwaysClamp);
    };
    distance("Drop to 10 TPS after", "##half-start", draft.half_rate_start, 1, 9999);
    distance("Drop to 5 TPS after", "##low-start", draft.low_rate_start, 2, 10000);
    distance("Back to full rate within", "##full-return", draft.full_rate_return, 0, 9998);
    distance("Back to 10 TPS within", "##half-return", draft.half_rate_return, 1, 9999);
    ImGui::EndDisabled();
    note("The return distances sit a little inside the drop ones, so a skater on the edge does not flip between rates.");
    const bool valid = draft.valid();
    if (!valid)
        warn("Keep the distances in order: back to full rate < drop to 10 <= back to 10 < drop to 5.");
    const int pressed = button_row(menu, "distance-actions", {{"APPLY TO LOBBY", editable && valid && !(draft == mp.distances)},
                                                              {"RESET VALUES", editable}});
    if (pressed == 0) {
        std::array<char, 65> unused{};
        send_private(menu, "distances", std::to_string(draft.full_rate_return) + " " +
            std::to_string(draft.half_rate_start) + " " + std::to_string(draft.half_rate_return) + " " +
            std::to_string(draft.low_rate_start), unused, false);
    }
    if (pressed == 1) draft = {};
    const auto rates = "Rates: " + std::to_string(mp.tps) + " / 10 / 5 TPS. Players who join later get these too.";
    note(rates.c_str());
    end_card();
}
void map_pool_settings(SkateMenu &menu, const Model &model) { // the server's own maps, not this PC's
    const auto &mp = model.multiplayer;
    if (!(server_can(mp, "map-pool") || server_can(mp, "rotation")) || mp.server_maps.empty()) return;
    std::array<char, 65> unused{};
    const auto server = [&](const std::string &command) { return send_private(menu, "server", command, unused, false); };
    const auto &pool = mp.server_map_pool;
    const auto pooled = pool.empty() ? mp.server_maps.size() : pool.size();
    const auto count = std::to_string(pooled) + " of " + std::to_string(mp.server_maps.size()) + " maps";
    begin_card(menu, "map-pool", "MAP POOL", count.c_str());
    note("The maps players can vote for, in rotation order: the server's own retail maps and the custom maps in its Mods folder.");
    bool votes = mp.server_map_votes;
    if (toggle_row(menu, "Player map votes", "Players start a vote with /vote map <map>.", votes))
        server(votes ? "votes map on" : "votes map off");
    ImGui::Dummy(ImVec2(0, px(2)));

    const auto now = ImGui::GetTime();
    std::erase_if(menu.map_pool_pending, [&](const auto &item) {
        const bool listed = pool.empty() || std::find(pool.begin(), pool.end(), item.first) != pool.end();
        return listed == item.second.first || now >= item.second.second;
    });
    const auto folded = [](std::string_view text) {
        std::string result(text);
        for (auto &c : result) c = c == '\\' ? '/' : c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c;
        return result;
    };
    for (const auto &asset : mp.server_maps) {
        const auto at = std::find(pool.begin(), pool.end(), asset);
        bool listed = pool.empty() || at != pool.end();
        if (const auto pending = menu.map_pool_pending.find(asset); pending != menu.map_pool_pending.end())
            listed = pending->second.first;
        auto label = map_label(model, asset);
        if (at != pool.end()) label = std::to_string(at - pool.begin() + 1) + ".  " + label;
        const bool installed = std::any_of(model.levels.begin(), model.levels.end(),
                                           [&](const auto &level) { return folded(level.asset) == folded(asset); });
        const auto hint = std::string(installed ? "" : "Not installed on this PC: you need its map mod to load it. ") +
                          (listed ? "Untick to take it out of votes and the rotation." : "Tick to add it to the end of the rotation.");
        const bool last = listed && pooled == 1; // the pool keeps at least one map
        ImGui::PushID(asset.c_str());
        if (toggle_row(menu, label.c_str(), last ? "The pool needs at least one map." : hint.c_str(), listed, !last, "ONLY") &&
            server(std::string(listed ? "map-pool add " : "map-pool remove ") + asset))
            menu.map_pool_pending[asset] = {listed, now + 2};
        ImGui::PopID();
    }
    if (!pool.empty() && button_row(menu, "every-map", {{"USE EVERY MAP"}}) == 0) server("map-pool clear");
    note(pool.empty() ? "Every map is in the pool. Untick maps to leave them out."
                      : "Ticked maps join the end of the rotation; the numbers are its order.");
    ImGui::Dummy(ImVec2(0, px(2)));

    field(menu, "Rotation", "Minutes on each map before the server moves to the next map in the pool. 0 turns it off.");
    int minutes = menu.map_rotation_pending.value_or(static_cast<int>(mp.server_map_rotation));
    set_console_step(5);
    ::dingosdk::overlay::menu::slider_int("##map-rotation", &minutes, 0, static_cast<int>(max_map_rotation), minutes ? "Every %d min" : "Off",
                                          ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemActive()) menu.map_rotation_pending = minutes;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        server(minutes ? "rotation " + std::to_string(minutes) : std::string("rotation off"));
        menu.map_rotation_until = now + 2;
    }
    if (!ImGui::IsItemActive() && menu.map_rotation_pending &&
        (*menu.map_rotation_pending == static_cast<int>(mp.server_map_rotation) || now >= menu.map_rotation_until))
        menu.map_rotation_pending.reset();
    note(mp.server_map_rotation ? "Players get a minute's warning. The clock waits while nobody is on and starts over when the map changes."
                                : "Off: the map changes only by a vote or an admin.");
    end_card();
}
} // namespace

void voice_controls(SkateMenu &menu, const MultiplayerModel &mp) {
    std::array<char, 65> unused{};
    if (menu.voice_pending && (*menu.voice_pending == mp.voice.settings || ImGui::GetTime() >= menu.voice_pending_until))
        menu.voice_pending.reset();
    auto value = menu.voice_pending.value_or(mp.voice.settings);
    bool changed{};

    // The lobby's rules, for the host only, apart from everyone's own settings below.
    if (controls(mp, "voice") || controls(mp, "voice-range")) {
        begin_card(menu, mp.dedicated ? "SERVER VOICE" : "LOBBY VOICE", mp.dedicated ? "SERVER VOICE" : "LOBBY VOICE",
                   mp.dedicated ? "Server settings for everyone" : "Host settings for everyone");
        bool allowed = mp.voice.allowed;
        if (toggle_row(menu, "Allow voice chat in this lobby", "Turn voice off for everyone in your lobby.", allowed))
            send_private(menu, "voice-allow", allowed ? "on" : "off", unused, false);
        field(menu, "Voice range", "How far apart players can be and still hear each other. Past this the host "
                                    "stops sending their voice.");
        float range = menu.voice_range_pending.value_or(mp.voice_range);
        ::dingosdk::overlay::menu::slider_float("##voice-range", &range, min_voice_range, max_voice_range, "%.0f m",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemActive()) menu.voice_range_pending = range;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            send_private(menu, "voice-range", std::to_string(static_cast<int>(range)), unused, false);
            menu.voice_range_until = ImGui::GetTime() + 2;
        }
        if (!ImGui::IsItemActive() && menu.voice_range_pending &&
            (*menu.voice_range_pending == mp.voice_range || ImGui::GetTime() >= menu.voice_range_until))
            menu.voice_range_pending.reset();
        note("Past the range nobody hears each other; closer in, each player's own hearing distance fades voices out.");
        end_card();
    }

    begin_card(menu, "voice", "TALKING", mp.hosting ? "Your own settings" : nullptr);
    if (!mp.hosting && !mp.voice.allowed && mp.active) warn("The host has turned voice chat off for this lobby.");
    changed |= toggle_row(menu, "Voice chat", "Talk to other players. Uses your Steam microphone settings.",
                          value.enabled);
    changed |= toggle_row(menu, "Open microphone", "Transmit whenever you speak, without push-to-talk.",
                          value.open_mic);
    if (value.open_mic) menu.voice_bind_capture = 0;
    ImGui::Dummy(ImVec2(0, px(2)));

    ImGui::BeginDisabled(value.open_mic);
    ControllerInput controller;
    DingoSDKOverlayReadControllerInput(&controller, true);
    DWORD foreground_process{};
    GetWindowThreadProcessId(GetForegroundWindow(), &foreground_process);
    if (menu.voice_bind_capture && (ImGui::GetTime() >= menu.voice_capture_until || foreground_process != GetCurrentProcessId()))
        menu.voice_bind_capture = 0;
    // Each bind as a tile that records a new one, and CLEAR beside it.
    field(menu, "Push-to-talk key");
    const auto key_label = menu.voice_bind_capture == 1 ? std::string("PRESS A KEY...") : voice_key_name(value.push_to_talk);
    const int key_pressed = button_row(menu, "voice-key", {{key_label.c_str(), true, false, "A, then the key to talk with."}, {"CLEAR"}});
    if (key_pressed == 0) {
        menu.voice_bind_capture = 1;
        menu.voice_capture_until = ImGui::GetTime() + 30;
        detail::OverlayInputAccess access;
        for (int key = 0; key < 256; ++key) menu.voice_keys_down[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    if (key_pressed == 1) { value.push_to_talk = 0; changed = true; menu.voice_bind_capture = 0; }
    field(menu, "Controller combo");
    const auto pad_label = menu.voice_bind_capture == 2 ? std::string("RECORDING...") : controller_combo_label(value.controller_combo, controller.style);
    const int pad_pressed = button_row(menu, "voice-controller", {{pad_label.c_str(), true, false, "A, then hold the button or combo to talk with."}, {"CLEAR"}});
    if (pad_pressed == 0) {
        menu.voice_bind_capture = 2;
        menu.voice_controller_capture = {};
        menu.voice_capture_until = ImGui::GetTime() + 30;
    }
    if (pad_pressed == 1) { value.controller_combo = 0; changed = true; menu.voice_bind_capture = 0; }
    if (menu.voice_bind_capture) {
        detail::OverlayInputAccess access;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) menu.voice_bind_capture = 0;
        if (menu.voice_bind_capture == 1) {
            for (int key = 1; key < 256; ++key) {
                const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
                const bool pressed = down && !menu.voice_keys_down[key];
                menu.voice_keys_down[key] = down;
                if (!pressed || key == VK_LBUTTON || key == VK_RBUTTON || key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) continue;
                menu.voice_bind_capture = 0;
                value.push_to_talk = key; changed = true;
                break;
            }
        } else if (menu.voice_bind_capture == 2) {
            if (const auto combo = menu.voice_controller_capture.update(controller)) {
                value.controller_combo = *combo; changed = true; menu.voice_bind_capture = 0;
            }
        }
        if (menu.voice_bind_capture) {
            note(menu.voice_bind_capture == 1 ? "Press a key, middle mouse or a side mouse button. Escape cancels." :
                 !controller.available ? "Connect a controller. Escape cancels." :
                 !menu.voice_controller_capture.ready ? "Release all controller buttons first." :
                 "Hold your button or combo, then release it to save. Escape cancels.");
            if (button_row(menu, "voice-cancel", {{"CANCEL BINDING"}}) == 0) menu.voice_bind_capture = 0;
        }
    }
    if (!value.push_to_talk && !value.controller_combo && !value.open_mic)
        warn("Assign a key or controller combo to talk, or turn on Open microphone.");
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, px(2)));

    // Steam records the voice, so the input device is the one chosen in Steam;
    // its API cannot be pointed at another.
    field(menu, "Input device", "Voice is recorded by Steam, so it uses the microphone chosen in Steam's voice settings.");
    if (button_row(menu, "voice-device", {{"CHOOSE IN STEAM VOICE SETTINGS"}}) == 0)
        ShellExecuteW(nullptr, L"open", L"steam://settings/voice", nullptr, nullptr, SW_SHOWNORMAL);
    field(menu, "Your microphone", "How loud other players hear you.");
    changed |= ::dingosdk::overlay::menu::slider_float("##microphone", &value.microphone, 0.f, max_voice_volume, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    if (mp.voice.transmitting) {
        tag(menu, "TRANSMITTING", skate_theme::good);
        ImGui::SameLine();
    }
    note(mp.voice.status.c_str());
    note("The microphone pauses while this menu is open or the game is in the background. Big boosts can distort.");
    end_card();

    begin_card(menu, "hearing", "HEARING");
    field(menu, "Voices volume", "How loud you hear everyone. Adjust single players below.");
    changed |= ::dingosdk::overlay::menu::slider_float("##listening", &value.volume, 0.f, max_voice_volume, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    changed |= toggle_row(menu, "Proximity voice", "Hear players quieter the further away they are. Off: everyone at full volume.",
                          value.proximity);
    ImGui::BeginDisabled(!value.proximity);
    field(menu, "Hearing distance", "How far away you hear nearby players. Voices get quieter with distance and "
                                    "fade out towards this edge.");
    changed |= ::dingosdk::overlay::menu::slider_float("##distance", &value.distance, min_hearing_distance, max_hearing_distance, "%.0f m",
                                  ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    end_card();

    if (changed) {
        if (send_private(menu, "voice", std::to_string(value.enabled) + " " + std::to_string(value.proximity) + " " +
            std::to_string(value.push_to_talk) + " " + std::to_string(value.distance) + " " + std::to_string(value.volume) + " " +
            std::to_string(value.open_mic) + " " + std::to_string(value.controller_combo) + " " + std::to_string(value.microphone), unused, false)) {
            menu.voice_pending = value;
            menu.voice_pending_until = ImGui::GetTime() + 2;
        }
    }

    // Each player a row: their volume at its end, and A for the slider, mute and ignore.
    begin_card(menu, "voice-players", "PLAYER VOLUMES", "Only changes what you hear");
    if (mp.roster.empty()) note("Players appear here once you are in a session with them.");
    for (const auto &peer : mp.roster) {
        const auto player = voice_of(mp, peer.id);
        float gain = player.volume;
        if (const auto pending = menu.voice_volume_pending.find(peer.id); pending != menu.voice_volume_pending.end()) gain = pending->second.first;
        OpenRow row;
        row.name = peer.name;
        std::array<char, 32> volume{};
        std::snprintf(volume.data(), volume.size(), "%.2fx", gain);
        row.side = volume.data();
        if (player.muted) row.tags.push_back({"MUTED", skate_theme::danger});
        else if (mp.voice.allowed && player.speaking) row.tags.push_back({"SPEAKING", skate_theme::good});
        if (peer.ignored) row.tags.push_back({"IGNORED", skate_theme::grey_text});
        row.hint = "A for this player's volume, mute and ignore.";
        if (open_row(menu, std::to_string(peer.id).c_str(), row)) {
            const int action = action_chips(menu, "voice-actions",
                {{player.muted ? "UNMUTE" : "MUTE", true, player.muted, "Stop hearing their voice, on your game only."},
                 {peer.ignored ? "UNIGNORE" : "IGNORE", true, peer.ignored,
                  "Hide this player and everything they do, on your screen only: their skater, voice, chat, invites and plugins. They are not told."}});
            if (action == 0) send_private(menu, "voice-mute", std::to_string(peer.id) + (player.muted ? " off" : " on"), unused, false);
            // Ignore: their skater, voice, chat and everything else of theirs, on this screen only.
            if (action == 1) send_private(menu, "ignore", std::to_string(peer.id) + (peer.ignored ? " off" : " on"), unused, false);
            player_volume(menu, mp, peer);
            end_open_row();
        }
    }
    end_card();
}

void ban_popup(SkateMenu &menu) {
    constexpr auto title = "Ban player###multiplayer-ban";
    if (menu.ban_confirm_requested) {
        ImGui::OpenPopup(title);
        menu.ban_confirm_requested = false;
    }
    if (!ImGui::IsPopupOpen(title)) return;
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(.5f, .5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(px(460), viewport->WorkSize.x - 24.f), 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(16)));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        ImGui::PushFont(menu.bold);
        ImGui::TextWrapped("Ban %s?", menu.ban_confirm_name.empty() ? std::to_string(menu.ban_confirm_id).c_str()
                                                                     : menu.ban_confirm_name.c_str());
        ImGui::PopFont();
        const bool server = menu.ban_confirm_on_server;
        note(server ? "They are removed now and can never join this server again. Unban them any time in Online > Bans."
                    : "They are removed now and can never join a lobby you host, even with your join code. "
                      "Unban them any time in Online > Bans.");
        ImGui::Dummy(ImVec2(0, px(4)));
        // The focus starts on BAN, so A bans; B (or Escape) cancels.
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const int pressed = button_row(menu, "ban-actions", {{"BAN", true, true}, {"CANCEL"}});
        const bool ban = pressed == 0;
        const bool cancel = pressed == 1 || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        if (ban) {
            std::array<char, 65> unused{};
            send_private(menu, "ban", std::to_string(menu.ban_confirm_id) + " " + menu.ban_confirm_name, unused, false);
        }
        if (ban || cancel) {
            menu.ban_confirm_id = 0;
            menu.ban_confirm_name.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}
// The players this PC ignores, here or not, each a row with UNIGNORE; and the players here to ignore.
void ignored_card(SkateMenu &menu, const MultiplayerModel &mp) {
    std::array<char, 65> unused{};
    const auto count = std::to_string(mp.ignored.size()) + (mp.ignored.size() == 1 ? " player" : " players");
    begin_card(menu, "ignored", "IGNORED PLAYERS", count.c_str());
    note("Only for you: nothing of theirs shows or is heard on your game, in any session, and they are not told. "
         "/ignore <player> in chat does it too.");
    if (mp.ignored.empty()) note("You ignore nobody.");
    for (const auto &player : mp.ignored) {
        OpenRow row;
        row.name = player.name.empty() ? std::string("Unknown player") : player.name;
        row.detail = std::to_string(player.id);
        if (std::any_of(mp.roster.begin(), mp.roster.end(), [&](const auto &p) { return p.id == player.id && p.connected; }))
            row.tags.push_back({"HERE", skate_theme::warning});
        row.hint = "A to unignore this player.";
        if (open_row(menu, ("ignored-" + std::to_string(player.id)).c_str(), row)) {
            if (action_chips(menu, "unignore", {{"UNIGNORE", true, false, "See and hear them again."}}) == 0)
                send_private(menu, "ignore", std::to_string(player.id) + " off", unused, false);
            end_open_row();
        }
    }
    // The players in this session you could ignore.
    bool heading{};
    for (const auto &peer : mp.roster) {
        if (peer.id == mp.local_id || !peer.connected || peer.ignored || (mp.dedicated && peer.id == mp.host_id)) continue;
        if (!std::exchange(heading, true)) {
            ImGui::Dummy(ImVec2(0, px(4)));
            note("Players here: ignore one from here, or from their row on the SESSION tab.");
        }
        OpenRow row;
        row.name = peer.name;
        row.hint = "A to ignore this player.";
        if (open_row(menu, ("here-" + std::to_string(peer.id)).c_str(), row)) {
            if (action_chips(menu, "ignore", {{"IGNORE", true, false,
                    "Hide this player and everything they do, on your screen only: their skater, voice, chat, invites and plugins."}}) == 0)
                send_private(menu, "ignore", std::to_string(peer.id) + " on", unused, false);
            end_open_row();
        }
    }
    end_card();
}

void bans_page(SkateMenu &menu, const Model &model) {
    ignored_card(menu, model.multiplayer);
    const auto &bans = model.multiplayer.bans;
    // An admin of a dedicated server manages the server's list; otherwise, this PC's.
    const bool server = model.multiplayer.server_bans;
    std::array<char, 65> unused{};
    const auto total = server ? model.multiplayer.server_ban_total : static_cast<unsigned>(bans.size());
    const auto count = std::to_string(total) + (total == 1 ? " player" : " players");
    begin_card(menu, "bans", server ? "SERVER BANS" : "BANNED PLAYERS", count.c_str());
    if (server) note("You are an admin here, so this is the server's own ban list. Your own list is back when you leave.");
    if (server && total > bans.size())
        note(("Showing the newest " + std::to_string(bans.size()) + ". Unban older ones with mp server unban <id>.").c_str());
    if (bans.empty())
        note(server ? "Nobody is banned from this server. Ban a player from the Session tab, or add one below."
                    : "Nobody is banned. Ban a player from the Session tab while you host, or add one below.");
    for (const auto &ban : bans) {
        OpenRow row;
        row.name = ban.name.empty() ? std::string("Unknown player") : ban.name;
        row.detail = std::to_string(ban.id);
        row.side = ban_date(ban.added);
        row.hint = "A to unban this player.";
        if (open_row(menu, ("ban-" + std::to_string(ban.id)).c_str(), row)) {
            if (action_chips(menu, "unban", {{"UNBAN", true, false, server ? "Let them join this server again." : "Let them join your lobbies again."}}) == 0)
                send_private(menu, "unban", std::to_string(ban.id), unused, false);
            end_open_row();
        }
    }
    end_card();

    begin_card(menu, "ban-add", server ? "BAN FROM THIS SERVER" : "BAN BY STEAM ID");
    field(menu, "Steam ID", "The 17-digit SteamID64, starting 7656119.");
    ImGui::InputTextWithHint("##ban-id", "7656119...", menu.ban_id.data(), menu.ban_id.size(), ImGuiInputTextFlags_CharsDecimal);
    field(menu, "Name", server ? "Optional; shown in the server's list." : "Optional; only for your own list.");
    ImGui::InputTextWithHint("##ban-name", "Optional", menu.ban_name.data(), menu.ban_name.size());
    ImGui::Dummy(ImVec2(0, px(2)));
    // Checked here too: a queued command's refusal never comes back to the menu.
    const std::string typed(menu.ban_id.data());
    std::uint64_t id{};
    const auto parsed = std::from_chars(typed.data(), typed.data() + typed.size(), id);
    const bool valid = parsed.ec == std::errc{} && parsed.ptr == typed.data() + typed.size() &&
                       id >= 76561197960265728ULL && id <= 76561202255233023ULL;
    const bool self = valid && id == model.multiplayer.local_id;
    const bool known = valid && std::any_of(bans.begin(), bans.end(), [&](const auto &ban) { return ban.id == id; });
    if (!typed.empty() && !valid) warn("That is not a SteamID64: it is 17 digits and starts with 7656119.");
    else if (self) warn("That is your own Steam ID.");
    else if (known) note("That player is already banned.");
    if (primary_button(menu, "BAN PLAYER", valid && !self && !known)) {
        if (send_private(menu, "ban", std::string(menu.ban_id.data()) + " " + menu.ban_name.data(), unused, false)) {
            menu.ban_id.fill(0);
            menu.ban_name.fill(0);
        }
    }
    note(server ? "Banned players can never join this server. A SteamID64 is the long number in a Steam profile's address."
                : "Banned players can never join a lobby you host, even with your join code. A SteamID64 is the long number "
                  "in a Steam profile's address.");
    end_card();
}
void session_page(SkateMenu &menu, const Model &model) {
    const auto &mp = model.multiplayer;
    std::array<char, 65> unused{};
    // The session: its name, then where and how full, as one line over its tags.
    const auto where = map_label(model, mp.map) +
                       (mp.active ? "  -  " + std::to_string(mp.players) + " of " + std::to_string(mp.capacity) : std::string{});
    begin_card(menu, "session", mp.lobby_name.empty() ? "CURRENT SESSION" : mp.lobby_name.c_str(), where.c_str());
    if (mp.lobby_joining) tag(menu, "CONNECTING", skate_theme::warning);
    else tag(menu, mp.hosting ? "HOST" : "GUEST", mp.hosting ? skate_theme::blue : skate_theme::good);
    if (mp.dedicated) {
        ImGui::SameLine(0, px(6));
        tag(menu, "DEDICATED SERVER", skate_theme::blue);
        if (!mp.server_role_tag.empty() || mp.server_admin) {
            // Their role on the server, as its tag reads (or its name, for a role without one).
            std::string role = mp.server_role_tag.empty() ? mp.server_role : mp.server_role_tag;
            for (auto &c : role) c = c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
            ImGui::SameLine(0, px(6));
            tag(menu, role.c_str(), skate_theme::warning);
        }
    }
    if (mp.hosting) {
        ImGui::SameLine(0, px(6));
        tag(menu, mp.public_host ? "PUBLIC" : "JOIN CODE", skate_theme::white);
        if (mp.password_required) {
            ImGui::SameLine(0, px(6));
            tag(menu, "PASSWORD", skate_theme::warning);
        }
    }
    note(mp.status.c_str());
    const bool code = mp.hosting && !mp.invite.empty();
    if (code) info(menu, "Join code", mp.invite);
    if (!mp.lobby_status.empty()) note(mp.lobby_status.c_str());
    ImGui::Dummy(ImVec2(0, px(2)));
    const char *leave = mp.lobby_joining ? "CANCEL JOIN" : mp.hosting ? "END SESSION" : "LEAVE SESSION";
    // 0 copies the join code, 1 leaves.
    int pressed = -1;
    if (code) pressed = button_row(menu, "session-actions", {{"COPY JOIN CODE", true, false, "Friends paste it in Servers > Join with a code."}, {leave}});
    else if (button_row(menu, "session-actions", {{leave}}) == 0) pressed = 1;
    if (code && pressed == 0) {
        ImGui::SetClipboardText(mp.invite.c_str());
        feedback(menu, "Join code copied.");
    }
    if (pressed == 1) send_private(menu, "stop", "", menu.multiplayer_join_password, false);
    end_card();
    if (!mp.active) return;

    // The players as rows: a square that is green once they are in the world, their tags, how far away
    // they are. A on a player opens what can be done with them under the row.
    const auto count = std::to_string(mp.players) + " / " + std::to_string(mp.capacity);
    begin_card(menu, "players", "PLAYERS", count.c_str());
    const auto copy_id = [&](std::uint64_t id) {
        ImGui::SetClipboardText(std::to_string(id).c_str());
        feedback(menu, "Steam ID copied.");
    };
    {
        // You. The ReSkate team's lists see everyone's Steam ID, and copy their own from here.
        OpenRow row;
        row.name = mp.local_name.empty() ? std::string("You") : mp.local_name;
        row.swatch = mp.local_ready ? skate_theme::good : skate_theme::warning;
        row.tags.push_back({"YOU", skate_theme::white});
        if (mp.hosting) row.tags.push_back({"HOST", skate_theme::blue});
        if (!mp.local_ready) row.tags.push_back({"LOADING", skate_theme::warning});
        if (mp.steam_ids_shown && mp.local_id) row.detail = std::to_string(mp.local_id);
        row.openable = mp.steam_ids_shown && mp.local_id;
        row.hint = "A to copy your Steam ID.";
        if (open_row(menu, "you", row)) {
            if (action_chips(menu, "you-actions", {{"COPY STEAM ID"}}) == 0) copy_id(mp.local_id);
            end_open_row();
        }
    }
    for (const auto &peer : mp.roster) {
        const bool in_game = peer.connected && peer.visible;
        const bool host = peer.id == mp.host_id;
        const auto voice = voice_of(mp, peer.id);
        OpenRow row;
        row.name = peer.name;
        row.swatch = in_game ? skate_theme::good : skate_theme::warning;
        if (host) row.tags.push_back({"HOST", skate_theme::blue});
        if (peer.party_member) row.tags.push_back({peer.party_leader ? "PARTY LEAD" : "PARTY", skate_theme::friends_text});
        if (!in_game) row.tags.push_back({!peer.connected ? "CONNECTING" : "LOADING", skate_theme::warning});
        if (voice.muted) row.tags.push_back({"MUTED", skate_theme::danger});
        if (peer.ignored) row.tags.push_back({"IGNORED", skate_theme::grey_text});
        if (peer.distance_m >= 0) row.side = std::to_string(static_cast<int>(peer.distance_m)) + " m";
        if (mp.steam_ids_shown && peer.id) row.detail = std::to_string(peer.id);
        row.hint = "A for teleport, mute, ignore, volume and the rest.";
        if (!open_row(menu, std::to_string(peer.id).c_str(), row)) continue;
        // (A dedicated server's own entry is not a skater to go to.)
        const int action = action_chips(menu, "player-actions",
            {{"TELEPORT", peer.connected && !(mp.dedicated && host), false, "Go to where they are skating."},
             {voice.muted ? "UNMUTE" : "MUTE", true, voice.muted, "Stop hearing their voice, on your game only."},
             // Ignoring them: nothing of theirs shows or is heard on this PC (the list is on the BANS tab).
             {peer.ignored ? "UNIGNORE" : "IGNORE", true, peer.ignored,
              peer.ignored ? "You ignore this player. Press to see and hear them again."
                           : "Hide this player and everything they do, on your screen only: their skater, voice, chat, invites and plugins. They are not told."},
             // Their placed objects: ghosts for you (skated and walked through) until made solid.
             {"SOLID OBJECTS", true, peer.objects_solid,
              peer.objects_solid ? "Everything this player places is solid for you. Press to pass through their objects instead."
                                 : "You pass through everything this player places. Press to make their objects solid, to skate them."}});
        if (action == 0) send_private(menu, "tp", std::to_string(peer.id), unused, false);
        if (action == 1) send_private(menu, "voice-mute", std::to_string(peer.id) + (voice.muted ? " off" : " on"), unused, false);
        if (action == 2) send_private(menu, "ignore", std::to_string(peer.id) + (peer.ignored ? " off" : " on"), unused, false);
        if (action == 3) send_private(menu, "objects-solid", std::to_string(peer.id) + (peer.objects_solid ? " off" : " on"), unused, false);
        // The host's (or a server role's) actions.
        if ((controls(mp, "kick") || controls(mp, "ban")) && peer.id != mp.local_id && !host) {
            const int removed = action_chips(menu, "host-actions",
                {{"KICK", peer.connected && peer.epoch != 0, false,
                  mp.dedicated ? "Remove them until the server restarts." : "Remove them from this session. They can join your next one."},
                 {"BAN", true, false,
                  mp.dedicated ? "Remove them and keep them off this server for good." : "Remove them and keep them out of every lobby you host."}});
            if (removed == 0) send_private(menu, "kick", std::to_string(peer.id) + " " + std::to_string(peer.epoch), unused, false);
            if (removed == 1) {
                menu.ban_confirm_id = peer.id;
                menu.ban_confirm_name = peer.name;
                menu.ban_confirm_on_server = mp.dedicated;
                menu.ban_confirm_requested = true;
            }
        }
        if (mp.steam_ids_shown && peer.id && action_chips(menu, "id-actions", {{"COPY STEAM ID"}}) == 0) copy_id(peer.id);
        player_volume(menu, mp, peer);
        end_open_row();
    }
    ImGui::Dummy(ImVec2(0, px(2)));
    if (!mp.object_status.empty()) note(("Objects: " + mp.object_status).c_str());
    if (mp.hosting) note("Travel from Map > Travel to take everyone with you.");
    else if (server_can(mp, "map")) note("Pick a level in Levels or Map > Travel to move the whole server there.");
    end_card();

    if (mp.server_admin) {
        begin_card(menu, "server-command", "SERVER COMMAND", mp.server_all_commands ? "Admins only" : "Your role's commands");
        note(mp.server_all_commands ? "Any server console command, such as status, map grom or tps 60. Type help for the list; replies appear in chat."
                                    : "The server commands your role has: type help for the list. Replies appear in chat.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        const bool enter = ImGui::InputTextWithHint("##server-command", "help", menu.server_command.data(),
                                                    menu.server_command.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        if ((button_row(menu, "server-send", {{"SEND", menu.server_command[0] != 0}}) == 0 || enter) && menu.server_command[0]) {
            send_private(menu, "server", menu.server_command.data(), unused, false);
            menu.server_command.fill(0);
        }
        end_card();
    }
    map_pool_settings(menu, model);

    begin_card(menu, "host-options", mp.dedicated ? "SERVER OPTIONS" : "HOST OPTIONS", mp.dedicated && mp.server_admin ? nullptr : set_by(mp));
    field(menu, "Who can build");
    int placement = mp.object_placement == ObjectPlacement::everyone ? 0 : mp.object_placement == ObjectPlacement::host_only ? 1 : 2;
    // On a dedicated server "host only" is its admins.
    if (choice(menu, "placement", placement, {"Everyone", mp.dedicated ? "Admins only" : "Host only", "Nobody"},
               controls(mp, "placement")))
        send_private(menu, "object-placement", placement == 0 ? "everyone" : placement == 1 ? "host" : "nobody", unused, false);
    note(mp.object_placement == ObjectPlacement::everyone ? "Everyone can place, move and delete objects."
         : mp.object_placement == ObjectPlacement::host_only
             ? (mp.dedicated ? (mp.server_admin ? "Only admins can build. Everyone else's editor is locked."
                                                : "Only the server's admins can build. Your editor and object tools are locked.")
                : mp.hosting ? "Only you can build. Guests' editors are locked and their objects stay as everyone sees them now."
                             : "Only the host can build. Your editor and object tools are locked.")
             : mp.dedicated ? "Nobody can build. Existing objects stay."
                            : "Nobody can build, including the host. Existing objects stay.");
    // How many objects each player may have placed: a few round numbers, and whatever the
    // server's file or console set that is not one of them (shown, not offered).
    {
        static constexpr std::array<unsigned, 7> limits{0, 10, 25, 50, 100, 250, 500};
        const auto found = std::find(limits.begin(), limits.end(), mp.object_limit);
        std::vector<std::string> labels{"No limit", "10", "25", "50", "100", "250", "500"};
        if (found == limits.end()) labels.push_back(std::to_string(mp.object_limit));
        int picked = static_cast<int>(found - limits.begin());
        if (option_row(menu, "Objects per player", "How many objects each player may have placed at once.", picked, labels,
                       controls(mp, "objects")) && picked < 7)
            send_private(menu, "object-limit", picked ? std::to_string(limits[static_cast<std::size_t>(picked)]) : std::string("off"), unused, false);
        const char *exempt = mp.dedicated ? "Admins" : "The host";
        if (!mp.object_limit) note("Everyone can place as many objects as they like.");
        else if (mp.object_limit_own)
            note(("You have placed " + std::to_string(mp.objects_placed) + " of " + std::to_string(mp.object_limit_own) +
                  " objects. Delete one to place another once you reach the limit.").c_str());
        else note((std::string("Each player can have ") + std::to_string(mp.object_limit) + " objects placed. " + exempt +
                   (mp.dedicated ? " are not limited." : " is not limited.")).c_str());
    }
    if (controls(mp, "clear-objects")) {
        if (button_row(menu, "clear-objects", {{mp.dedicated ? "DELETE ALL PLACED OBJECTS" : "DELETE ALL GUEST OBJECTS"}}) == 0)
            send_private(menu, "clear-objects", "", unused, false);
        note(mp.dedicated ? "Removes every object players placed, for everyone; their saved parks stay. Enforced by the server, "
                            "so modified clients cannot get around it."
                          : "Removes every object guests placed, for everyone; your objects and guests' saved parks stay. Enforced by "
                            "the host, so modified clients cannot get around it.");
    }
    ImGui::Dummy(ImVec2(0, px(2)));
    const char *who = mp.dedicated ? "Players" : "Guests";
    const char *exempt = mp.dedicated ? " Admins always can." : " You always can.";
    bool noclip = mp.guest_noclip;
    if (toggle_row(menu, "Allow noclip", (std::string(who) + " may fly with noclip and teleport." + exempt).c_str(), noclip,
                   controls(mp, "noclip"), noclip ? "ALLOWED" : "OFF"))
        send_private(menu, "noclip-allow", noclip ? "on" : "off", unused, false);
    bool no_bail = mp.guest_no_bail;
    if (toggle_row(menu, "Allow No Bail", (std::string(who) + " may turn on No Bail." + exempt).c_str(), no_bail,
                   controls(mp, "nobail"), no_bail ? "ALLOWED" : "OFF"))
        send_private(menu, "nobail-allow", no_bail ? "on" : "off", unused, false);
    bool boosts = mp.guest_boosts;
    if (toggle_row(menu, "Allow boosts", (std::string(who) + " may use the forward and up boosts." + exempt).c_str(), boosts,
                   controls(mp, "boosts"), boosts ? "ALLOWED" : "OFF"))
        send_private(menu, "boosts-allow", boosts ? "on" : "off", unused, false);
    bool tuning = mp.enforce_tuning;
    if (toggle_row(menu, "Enforce physics tuning",
                   mp.dedicated ? "Players skate with the game's own Gameplay/SkatePhysicsTuning, not their edited copies."
                                : "Guests skate with your Gameplay/SkatePhysicsTuning (truck positions and the rest), not their own.",
                   tuning, controls(mp, "tuning"), tuning ? "ON" : "OFF"))
        send_private(menu, "tuning-enforce", tuning ? "on" : "off", unused, false);
    if (!mp.tuning_status.empty()) note(mp.tuning_status.c_str());
    ImGui::Dummy(ImVec2(0, px(2)));
    bool force_layers = mp.force_world_layers;
    if (toggle_row(menu, "Sync world layers", "Guests follow your Map > Layers and crowds choices and cannot change their own.",
                   force_layers, controls(mp, "layer-sync"), force_layers ? "ON" : "OFF"))
        send_private(menu, "world-layer-sync", force_layers ? "on" : "off", unused, false);
    if (mp.dedicated)
        note("The server's layers are set with server commands: layer <key> on|off (keys are listed in Map > Layers and crowds).");
    else note(force_layers ? "Guests follow the host's world layers; their layer controls are locked."
                      : "Everyone picks their own world layers. Turning sync off restores each guest's own choices.");
    end_card();

    distance_settings(menu, mp);
}
} // namespace dingosdk::overlay::menu::multiplayer_detail
