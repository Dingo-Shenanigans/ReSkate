#include "multiplayer_menu_internal.h"
#include "input_capture.h"
#include "skate_style.h"
#include "Extension/UI/skate_theme.h"
#include "Engine/Game/Input/voice_input.h"
#include <algorithm>
#include <charconv>
#include <ctime>
#include <Windows.h>
#include <shellapi.h>

namespace dingosdk::overlay::menu::multiplayer_detail {
namespace {
// Two buttons sharing a row equally.
bool half_button(const char* label, bool first) {
    const float width = (ImGui::GetContentRegionAvail().x - (first ? ImGui::GetStyle().ItemSpacing.x : 0.0f)) *
                        (first ? .5f : 1.0f);
    return ImGui::Button(label, ImVec2(width, 0));
}
// A button filling the row after a label from field(), leaving room for a
// small trailing button.
bool bind_button(const std::string& label, const char* trailing) {
    const float trailing_width = ImGui::CalcTextSize(trailing).x + ImGui::GetStyle().FramePadding.x * 2;
    return ImGui::Button(label.c_str(), ImVec2(ImGui::GetContentRegionAvail().x - trailing_width -
                                                   ImGui::GetStyle().ItemSpacing.x, 0));
}

// Who may change the session's settings: its host, or an admin of the dedicated
// server it runs on (their changes are sent to the server).
bool controls_session(const MultiplayerModel &mp) { return mp.hosting || mp.server_admin; }
const char *set_by(const MultiplayerModel &mp) {
    return controls_session(mp) ? nullptr : mp.dedicated ? "Set by the server" : "Set by the host";
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
    begin_card(menu, "distances", "NETWORK DISTANCES", set_by(mp));
    note("Skaters further away update less often, which keeps big lobbies smooth.");
    ImGui::BeginDisabled(!controls_session(mp));
    field(menu, "Drop to 10 TPS after");
    ImGui::DragInt("##half-start", &draft.half_rate_start, 1.f, 1, 9999, "%d m", ImGuiSliderFlags_AlwaysClamp);
    field(menu, "Drop to 5 TPS after");
    ImGui::DragInt("##low-start", &draft.low_rate_start, 1.f, 2, 10000, "%d m", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();
    if (ImGui::TreeNode("Return distances")) {
        note("Slightly shorter than the drop distances, so a skater on the boundary does not flip between rates.");
        ImGui::BeginDisabled(!controls_session(mp));
        field(menu, "Back to full rate within");
        ImGui::DragInt("##full-return", &draft.full_rate_return, 1.f, 0, 9998, "%d m", ImGuiSliderFlags_AlwaysClamp);
        field(menu, "Back to 10 TPS within");
        ImGui::DragInt("##half-return", &draft.half_rate_return, 1.f, 1, 9999, "%d m", ImGuiSliderFlags_AlwaysClamp);
        ImGui::EndDisabled();
        ImGui::TreePop();
    }
    const bool valid = draft.valid();
    if (!valid)
        warn("Keep the distances in order: back to full rate < drop to 10 <= back to 10 < drop to 5.");
    ImGui::BeginDisabled(!controls_session(mp));
    ImGui::BeginDisabled(!valid || draft == mp.distances);
    if (half_button("Apply to lobby", true)) {
        std::array<char, 65> unused{};
        send_private(menu, "distances", std::to_string(draft.full_rate_return) + " " +
            std::to_string(draft.half_rate_start) + " " + std::to_string(draft.half_rate_return) + " " +
            std::to_string(draft.low_rate_start), unused, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (half_button("Reset values", false)) draft = {};
    ImGui::EndDisabled();
    const auto rates = "Rates: " + std::to_string(mp.tps) + " / 10 / 5 TPS. Players who join later get these too.";
    note(rates.c_str());
    end_card();
}
void map_pool_settings(SkateMenu &menu, const Model &model) { // the server's own maps, not this PC's
    const auto &mp = model.multiplayer;
    if (!mp.server_admin || mp.server_maps.empty()) return;
    std::array<char, 65> unused{};
    const auto server = [&](const std::string &command) { return send_private(menu, "server", command, unused, false); };
    const auto &pool = mp.server_map_pool;
    const auto pooled = pool.empty() ? mp.server_maps.size() : pool.size();
    const auto count = std::to_string(pooled) + " of " + std::to_string(mp.server_maps.size()) + " maps";
    begin_card(menu, "map-pool", "MAP POOL", count.c_str());
    note("The maps players can vote for, in the order the rotation goes through them. These are the server's own "
         "maps: its retail maps and the custom maps in its Mods folder.");
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
    if (!pool.empty() && ImGui::Button("Use every map", ImVec2(-FLT_MIN, 0))) server("map-pool clear");
    note(pool.empty() ? "Every map is in the pool. Untick maps to leave them out."
                      : "Ticked maps join the end of the rotation; the numbers are its order.");
    ImGui::Dummy(ImVec2(0, px(2)));

    field(menu, "Rotation", "Minutes on each map before the server moves to the next map in the pool. 0 turns it off.");
    int minutes = menu.map_rotation_pending.value_or(static_cast<int>(mp.server_map_rotation));
    ImGui::DragInt("##map-rotation", &minutes, .25f, 0, static_cast<int>(max_map_rotation), minutes ? "Every %d min" : "Off",
                   ImGuiSliderFlags_AlwaysClamp);
    if (ImGui::IsItemActive()) menu.map_rotation_pending = minutes;
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        server(minutes ? "rotation " + std::to_string(minutes) : std::string("rotation off"));
        menu.map_rotation_until = now + 2;
    }
    if (!ImGui::IsItemActive() && menu.map_rotation_pending &&
        (*menu.map_rotation_pending == static_cast<int>(mp.server_map_rotation) || now >= menu.map_rotation_until))
        menu.map_rotation_pending.reset();
    note(mp.server_map_rotation ? "Set to 0 to turn the auto rotation off. Players get a minute's warning; the clock "
                                  "waits while nobody is on, and starts over whenever the map changes."
                                : "Auto rotation is off (0). Drag above 0 to change the map on a timer; until then it "
                                  "changes only by a vote or an admin.");
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
    if (controls_session(mp)) {
        begin_card(menu, mp.dedicated ? "SERVER VOICE" : "LOBBY VOICE", mp.dedicated ? "SERVER VOICE" : "LOBBY VOICE",
                   mp.dedicated ? "Server settings for everyone" : "Host settings for everyone");
        bool allowed = mp.voice.allowed;
        if (toggle_row(menu, "Allow voice chat in this lobby", "Turn voice off for everyone in your lobby.", allowed))
            send_private(menu, "voice-allow", allowed ? "on" : "off", unused, false);
        field(menu, "Voice range", "How far apart players can be and still hear each other. Past this the host "
                                    "stops sending their voice.");
        float range = menu.voice_range_pending.value_or(mp.voice_range);
        ImGui::SliderFloat("##voice-range", &range, min_voice_range, max_voice_range, "%.0f m",
                           ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (ImGui::IsItemActive()) menu.voice_range_pending = range;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            send_private(menu, "voice-range", std::to_string(static_cast<int>(range)), unused, false);
            menu.voice_range_until = ImGui::GetTime() + 2;
        }
        if (!ImGui::IsItemActive() && menu.voice_range_pending &&
            (*menu.voice_range_pending == mp.voice_range || ImGui::GetTime() >= menu.voice_range_until))
            menu.voice_range_pending.reset();
        note("Players further apart than the voice range cannot hear each other. Closer in, each player's own "
             "hearing distance fades voices out.");
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
    field(menu, "Push-to-talk key");
    const auto key_label = (menu.voice_bind_capture == 1 ? std::string("Press a key...")
                                                         : voice_key_name(value.push_to_talk)) + "###voice-key";
    if (bind_button(key_label, "Clear")) {
        menu.voice_bind_capture = 1;
        menu.voice_capture_until = ImGui::GetTime() + 30;
        detail::OverlayInputAccess access;
        for (int key = 0; key < 256; ++key) menu.voice_keys_down[key] = (GetAsyncKeyState(key) & 0x8000) != 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##voice-key")) { value.push_to_talk = 0; changed = true; menu.voice_bind_capture = 0; }
    field(menu, "Controller combo");
    const auto pad_label = (menu.voice_bind_capture == 2 ? std::string("Recording...")
                                                         : controller_combo_label(value.controller_combo, controller.style)) +
                           "###voice-controller";
    if (bind_button(pad_label, "Clear")) {
        menu.voice_bind_capture = 2;
        menu.voice_controller_capture = {};
        menu.voice_capture_until = ImGui::GetTime() + 30;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##voice-controller")) { value.controller_combo = 0; changed = true; menu.voice_bind_capture = 0; }
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
            if (ImGui::SmallButton("Cancel binding")) menu.voice_bind_capture = 0;
        }
    }
    if (!value.push_to_talk && !value.controller_combo && !value.open_mic)
        warn("Assign a key or controller combo to talk, or turn on Open microphone.");
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, px(2)));

    // Steam records the voice, so the input device is the one chosen in Steam;
    // its API cannot be pointed at another.
    field(menu, "Input device", "Voice is recorded by Steam, so it uses the microphone chosen in Steam's voice settings.");
    if (ImGui::Button("Choose in Steam voice settings", ImVec2(-FLT_MIN, 0)))
        ShellExecuteW(nullptr, L"open", L"steam://settings/voice", nullptr, nullptr, SW_SHOWNORMAL);
    field(menu, "Your microphone", "How loud other players hear you.");
    changed |= ImGui::SliderFloat("##microphone", &value.microphone, 0.f, max_voice_volume, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    if (mp.voice.transmitting) {
        tag(menu, "TRANSMITTING", skate_theme::good);
        ImGui::SameLine();
    }
    note(mp.voice.status.c_str());
    note("The microphone pauses while this menu is open or the game is in the background. Big boosts can distort.");
    end_card();

    begin_card(menu, "hearing", "HEARING");
    field(menu, "Voices volume", "How loud you hear everyone. Adjust single players below.");
    changed |= ImGui::SliderFloat("##listening", &value.volume, 0.f, max_voice_volume, "%.2fx", ImGuiSliderFlags_AlwaysClamp);
    changed |= toggle_row(menu, "Proximity voice", "Hear players quieter the further away they are. Off: everyone at full volume.",
                          value.proximity);
    ImGui::BeginDisabled(!value.proximity);
    field(menu, "Hearing distance", "How far away you hear nearby players. Voices get quieter with distance and "
                                    "fade out towards this edge.");
    changed |= ImGui::SliderFloat("##distance", &value.distance, min_hearing_distance, max_hearing_distance, "%.0f m",
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

    begin_card(menu, "voice-players", "PLAYER VOLUMES", "Only changes what you hear");
    std::erase_if(menu.voice_volume_pending, [&](const auto &item) {
        return ImGui::GetTime() >= item.second.second;
    });
    if (mp.roster.empty()) note("Players appear here once you are in a session with them.");
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(8), px(6)));
    if (!mp.roster.empty() && ImGui::BeginTable("voice-players", 4,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Player");
        ImGui::TableSetupColumn("Volume");
        ImGui::TableSetupColumn("##state", ImGuiTableColumnFlags_WidthFixed, px(100));
        ImGui::TableSetupColumn("##mute", ImGuiTableColumnFlags_WidthFixed, px(76));
        ImGui::TableHeadersRow();
        for (const auto &peer : mp.roster) {
            ImGui::PushID(std::to_string(peer.id).c_str());
            const auto found = std::find_if(mp.voice.players.begin(), mp.voice.players.end(), [&](const auto &p) { return p.id == peer.id; });
            const auto player = found == mp.voice.players.end() ? VoicePlayer{peer.id} : *found;
            auto gain = player.volume;
            if (const auto pending = menu.voice_volume_pending.find(peer.id); pending != menu.voice_volume_pending.end()) {
                if (pending->second.first == gain) menu.voice_volume_pending.erase(pending);
                else gain = pending->second.first;
            }
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding(); cell_text(peer.name);
            ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderFloat("##volume", &gain, 0.f, max_voice_volume, "%.2fx", ImGuiSliderFlags_AlwaysClamp) &&
                send_private(menu, "voice-volume", std::to_string(peer.id) + " " + std::to_string(gain), unused, false))
                menu.voice_volume_pending[peer.id] = {gain, ImGui::GetTime() + 2};
            ImGui::TableNextColumn();
            if (player.muted) tag(menu, "MUTED", skate_theme::danger);
            else if (mp.voice.allowed && player.speaking) tag(menu, "SPEAKING", skate_theme::good);
            ImGui::TableNextColumn();
            if (ImGui::Button(player.muted ? "Unmute" : "Mute", ImVec2(-1, 0)))
                send_private(menu, "voice-mute", std::to_string(peer.id) + (player.muted ? " off" : " on"), unused, false);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
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
    ImGui::SetNextWindowSize(ImVec2(std::min(px(440), viewport->WorkSize.x - 24.f), 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(18), px(16)));
    if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize)) {
        ImGui::PushFont(menu.bold);
        ImGui::TextWrapped("Ban %s?", menu.ban_confirm_name.empty() ? std::to_string(menu.ban_confirm_id).c_str()
                                                                     : menu.ban_confirm_name.c_str());
        ImGui::PopFont();
        const bool server = menu.ban_confirm_on_server;
        note(server ? "They are removed now and can never join this server again. Unban them any time in Multiplayer > Bans."
                    : "They are removed now and can never join a lobby you host, even with your join code. "
                      "Unban them any time in Multiplayer > Bans.");
        ImGui::Dummy(ImVec2(0, px(4)));
        const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
        skate_theme::push_primary_button();
        const bool ban = ImGui::Button("Ban", ImVec2(half, 0));
        skate_theme::pop_primary_button();
        ImGui::SameLine();
        const bool cancel = ImGui::Button("Cancel", ImVec2(-FLT_MIN, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
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
void bans_page(SkateMenu &menu, const Model &model) {
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
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(8), px(6)));
    if (!bans.empty() && ImGui::BeginTable("bans-table", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Banned", ImGuiTableColumnFlags_WidthFixed, px(120));
        ImGui::TableSetupColumn("##unban", ImGuiTableColumnFlags_WidthFixed, px(90));
        ImGui::TableHeadersRow();
        for (const auto &ban : bans) {
            ImGui::PushID(std::to_string(ban.id).c_str());
            ImGui::TableNextRow(); ImGui::TableNextColumn();
            ImGui::PushFont(menu.bold);
            cell_text(ban.name.empty() ? std::string("Unknown player") : ban.name);
            ImGui::PopFont();
            ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
            ImGui::TextUnformatted(std::to_string(ban.id).c_str());
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            std::array<char, 32> date{};
            const auto added = static_cast<std::time_t>(ban.added);
            std::tm local{};
            if (ban.added > 0 && localtime_s(&local, &added) == 0) std::strftime(date.data(), date.size(), "%d %b %Y", &local);
            ImGui::TextUnformatted(date[0] ? date.data() : "-");
            ImGui::TableNextColumn();
            if (ImGui::Button("Unban", ImVec2(-1, 0))) send_private(menu, "unban", std::to_string(ban.id), unused, false);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
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
    note(server ? "Banned players can never join this server. A SteamID64 is the long number in a Steam profile's "
                  "address (steamcommunity.com/profiles/...)."
                : "Banned players can never join a lobby you host, even with your join code. A SteamID64 is the long number "
                  "in a Steam profile's address (steamcommunity.com/profiles/...).");
    end_card();
}
void session_page(SkateMenu &menu, const Model &model) {
    const auto &mp = model.multiplayer;
    begin_card(menu, "session", mp.lobby_name.empty() ? "CURRENT SESSION" : mp.lobby_name.c_str());
    if (mp.lobby_joining) tag(menu, "CONNECTING", skate_theme::warning);
    else tag(menu, mp.hosting ? "HOST" : "GUEST", mp.hosting ? skate_theme::blue : skate_theme::good);
    if (mp.dedicated) {
        ImGui::SameLine(0, px(6));
        tag(menu, "DEDICATED SERVER", skate_theme::blue);
        if (mp.server_admin) {
            ImGui::SameLine(0, px(6));
            tag(menu, "ADMIN", skate_theme::warning);
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
    ImGui::Dummy(ImVec2(0, px(2)));
    info(menu, "Map", map_label(model, mp.map));
    if (mp.active) info(menu, "Players", std::to_string(mp.players) + " / " + std::to_string(mp.capacity));
    if (mp.hosting && !mp.invite.empty()) {
        field(menu, "Join code", "Friends paste this in Servers > Join with a code.");
        std::array<char, 128> code{};
        std::copy_n(mp.invite.begin(), std::min(mp.invite.size(), code.size() - 1), code.begin());
        const float copy = ImGui::CalcTextSize("Copy").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - copy - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##join-code", code.data(), code.size(), ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::Button("Copy")) {
            ImGui::SetClipboardText(mp.invite.c_str());
            feedback(menu, "Join code copied.");
        }
    }
    if (!mp.lobby_status.empty()) note(mp.lobby_status.c_str());
    ImGui::Dummy(ImVec2(0, px(2)));
    if (ImGui::Button(mp.lobby_joining ? "Cancel join" : mp.hosting ? "End session" : "Leave session", ImVec2(-FLT_MIN, 0)))
        send_private(menu, "stop", "", menu.multiplayer_join_password, false);
    end_card();
    if (!mp.active) return;

    const auto count = std::to_string(mp.players) + " / " + std::to_string(mp.capacity);
    begin_card(menu, "players", "PLAYERS", count.c_str());
    auto *draw = ImGui::GetWindowDrawList();
    const auto player = [&](int index, const std::string &name, bool you, bool host, std::string detail,
                            const char *status, ImU32 colour, const MultiplayerPlayer *peer) {
        ImGui::PushID(index);
        const auto top = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x, height = px(54), pad = px(14);
        skate_theme::rough_rect(draw, top, ImVec2(top.x + width, top.y + height), skate_theme::tile_grey,
                                static_cast<unsigned>(index + 57), px(1));
        if (host) draw->AddRectFilled(top, ImVec2(top.x + px(4), top.y + height), skate_theme::blue);
        const float middle = top.y + (height - ImGui::GetFrameHeight()) * .5f;
        float right = top.x + width - pad;
        // Right: the host's actions, then the status tag leading into them.
        if (peer && controls_session(mp) && peer->id != mp.local_id && peer->id != mp.host_id) {
            const float button = px(76), spacing = ImGui::GetStyle().ItemSpacing.x;
            right -= button * 2 + spacing;
            ImGui::SetCursorScreenPos(ImVec2(right, middle));
            ImGui::BeginDisabled(!peer->connected || !peer->epoch);
            if (ImGui::Button("Kick", ImVec2(button, 0))) {
                std::array<char, 65> unused{};
                send_private(menu, "kick", std::to_string(peer->id) + " " + std::to_string(peer->epoch), unused, false);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", mp.dedicated ? "Remove them until the server restarts."
                                                     : "Remove them from this session. They can join your next one.");
            ImGui::SameLine();
            if (ImGui::Button("Ban", ImVec2(button, 0))) {
                menu.ban_confirm_id = peer->id;
                menu.ban_confirm_name = peer->name;
                menu.ban_confirm_on_server = mp.dedicated;
                menu.ban_confirm_requested = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", mp.dedicated ? "Remove them and keep them off this server for good."
                                                     : "Remove them and keep them out of every lobby you host.");
            right -= px(14);
        }
        const float status_width = menu.bold->CalcTextSizeA(px(12), FLT_MAX, 0, status).x + px(14);
        right -= status_width;
        ImGui::SetCursorScreenPos(ImVec2(right, middle));
        tag(menu, status, colour);
        right -= px(14);
        // Left: the name, with who they are under it.
        ImGui::SetCursorScreenPos(ImVec2(top.x + pad + (host ? px(4) : 0.0f), top.y + px(8)));
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(right, top.y + height), true);
        ImGui::BeginGroup();
        ImGui::PushFont(menu.bold);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopFont();
        if (you) {
            ImGui::SameLine(0, px(8));
            ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::blue);
            ImGui::TextUnformatted("you");
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, skate_theme::grey_text);
        ImGui::TextUnformatted(detail.c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();
        ImGui::PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(top.x, top.y + height + px(6)));
        ImGui::Dummy(ImVec2(0, 0));
        ImGui::PopID();
    };
    player(0, mp.local_name.empty() ? std::string("You") : mp.local_name, true, mp.hosting,
           mp.hosting ? "Host" : "Guest", mp.local_ready ? "IN GAME" : "LOADING",
           mp.local_ready ? skate_theme::good : skate_theme::warning, nullptr);
    // Only the tiles in view are drawn; every tile is the same height.
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(mp.roster.size()));
    while (clipper.Step()) {
        for (auto i = static_cast<std::size_t>(clipper.DisplayStart); i < static_cast<std::size_t>(clipper.DisplayEnd); ++i) {
            const auto &peer = mp.roster[i];
            const bool in_game = peer.connected && peer.visible;
            std::string detail = peer.id == mp.host_id ? "Host" : "Guest";
            if (peer.distance_m >= 0) detail += "  -  " + std::to_string(static_cast<int>(peer.distance_m)) + " m away";
            player(static_cast<int>(i) + 1, peer.name, false, peer.id == mp.host_id, std::move(detail),
                   !peer.connected ? "CONNECTING" : in_game ? "IN GAME" : "LOADING",
                   in_game ? skate_theme::good : skate_theme::warning, &peer);
        }
    }
    ImGui::Dummy(ImVec2(0, px(2)));
    if (!mp.object_status.empty()) note(("Objects: " + mp.object_status).c_str());
    if (mp.hosting) note("Travel from Map > Travel to take everyone with you.");
    else if (mp.server_admin) note("Pick a level in Levels or Map > Travel to move the whole server there.");
    end_card();

    if (mp.server_admin) {
        begin_card(menu, "server-command", "SERVER COMMAND", "Admins only");
        note("Any server console command: status, map grom, name <text>, tps 60, park historic megapark_02, "
             "layer-sync on... Type help for the list. Replies appear in chat.");
        const float send = ImGui::CalcTextSize("Send").x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - send - ImGui::GetStyle().ItemSpacing.x);
        const bool enter = ImGui::InputTextWithHint("##server-command", "help", menu.server_command.data(),
                                                    menu.server_command.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button("Send") || enter) && menu.server_command[0]) {
            std::array<char, 65> unused{};
            send_private(menu, "server", menu.server_command.data(), unused, false);
            menu.server_command.fill(0);
        }
        end_card();
    }
    map_pool_settings(menu, model);

    begin_card(menu, "host-options", mp.dedicated ? "SERVER OPTIONS" : "HOST OPTIONS", set_by(mp));
    std::array<char, 65> unused{};
    field(menu, "Who can build");
    int placement = mp.object_placement == ObjectPlacement::everyone ? 0 : mp.object_placement == ObjectPlacement::host_only ? 1 : 2;
    // On a dedicated server "host only" is its admins.
    if (choice(menu, "placement", placement, {"Everyone", mp.dedicated ? "Admins only" : "Host only", "Nobody"},
               controls_session(mp)))
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
    // server's file or console set that is not one of them.
    {
        static constexpr std::array<unsigned, 7> limits{0, 10, 25, 50, 100, 250, 500};
        const auto found = std::find(limits.begin(), limits.end(), mp.object_limit);
        const std::string custom = std::to_string(mp.object_limit);
        std::array<const char *, 8> labels{"No limit", "10", "25", "50", "100", "250", "500", custom.c_str()};
        int picked = static_cast<int>(found - limits.begin());
        field(menu, "Objects per player");
        ImGui::BeginDisabled(!controls_session(mp));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##object-limit", &picked, labels.data(), found == limits.end() ? 8 : 7) && picked < 7)
            send_private(menu, "object-limit", picked ? std::to_string(limits[static_cast<std::size_t>(picked)]) : std::string("off"), unused, false);
        ImGui::EndDisabled();
        const char *exempt = mp.dedicated ? "Admins" : "The host";
        if (!mp.object_limit) note("Everyone can place as many objects as they like.");
        else if (mp.object_limit_own)
            note(("You have placed " + std::to_string(mp.objects_placed) + " of " + std::to_string(mp.object_limit_own) +
                  " objects. Delete one to place another once you reach the limit.").c_str());
        else note((std::string("Each player can have ") + std::to_string(mp.object_limit) + " objects placed. " + exempt +
                   (mp.dedicated ? " are not limited." : " is not limited.")).c_str());
    }
    if (controls_session(mp)) {
        note(mp.dedicated ? "Enforced by the server, so modified clients cannot get around it."
                          : "Enforced by the host, so modified clients cannot get around it.");
        if (ImGui::Button(mp.dedicated ? "Delete all placed objects" : "Delete all guest objects", ImVec2(-FLT_MIN, 0)))
            send_private(menu, "clear-objects", "", unused, false);
        note(mp.dedicated ? "Removes every object players placed, for everyone. Their saved parks stay."
                          : "Removes every object guests placed, for everyone. Your objects and guests' saved parks stay.");
    }
    ImGui::Dummy(ImVec2(0, px(2)));
    const char *who = mp.dedicated ? "Players" : "Guests";
    const char *exempt = mp.dedicated ? " Admins always can." : " You always can.";
    bool noclip = mp.guest_noclip;
    if (toggle_row(menu, "Allow noclip", (std::string(who) + " may fly with noclip and teleport." + exempt).c_str(), noclip,
                   controls_session(mp), noclip ? "ALLOWED" : "OFF"))
        send_private(menu, "noclip-allow", noclip ? "on" : "off", unused, false);
    bool no_bail = mp.guest_no_bail;
    if (toggle_row(menu, "Allow No Bail", (std::string(who) + " may turn on No Bail." + exempt).c_str(), no_bail,
                   controls_session(mp), no_bail ? "ALLOWED" : "OFF"))
        send_private(menu, "nobail-allow", no_bail ? "on" : "off", unused, false);
    bool boosts = mp.guest_boosts;
    if (toggle_row(menu, "Allow boosts", (std::string(who) + " may use the forward and up boosts." + exempt).c_str(), boosts,
                   controls_session(mp), boosts ? "ALLOWED" : "OFF"))
        send_private(menu, "boosts-allow", boosts ? "on" : "off", unused, false);
    bool tuning = mp.enforce_tuning;
    if (toggle_row(menu, "Enforce physics tuning",
                   mp.dedicated ? "Players skate with the game's own Gameplay/SkatePhysicsTuning, not their edited copies."
                                : "Guests skate with your Gameplay/SkatePhysicsTuning (truck positions and the rest), not their own.",
                   tuning, controls_session(mp), tuning ? "ON" : "OFF"))
        send_private(menu, "tuning-enforce", tuning ? "on" : "off", unused, false);
    if (!mp.tuning_status.empty()) note(mp.tuning_status.c_str());
    ImGui::Dummy(ImVec2(0, px(2)));
    bool force_layers = mp.force_world_layers;
    if (toggle_row(menu, "Sync world layers", "Guests follow your World > Layers choices and cannot change their own.",
                   force_layers, controls_session(mp), force_layers ? "ON" : "OFF"))
        send_private(menu, "world-layer-sync", force_layers ? "on" : "off", unused, false);
    if (mp.dedicated)
        note("The server's layers are set with server commands: layer <key> on|off (keys are listed in World > Layers).");
    else note(force_layers ? "Guests follow the host's world layers; their layer controls are locked."
                      : "Everyone picks their own world layers. Turning sync off restores each guest's own choices.");
    end_card();

    distance_settings(menu, mp);
}
} // namespace dingosdk::overlay::menu::multiplayer_detail
