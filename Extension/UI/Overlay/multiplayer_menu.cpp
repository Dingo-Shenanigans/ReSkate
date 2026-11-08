#include "multiplayer_menu_internal.h"
#include "Extension/Boot/discord_presence.h"
#include "Engine/Game/World/world_names.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <format>
#include <Windows.h>

namespace dingosdk::overlay {
namespace {
std::atomic<MultiplayerQueue> private_queue{};
}
void set_multiplayer_queue(MultiplayerQueue value) noexcept { private_queue.store(value); }
} // namespace dingosdk::overlay

namespace dingosdk::overlay::detail {
bool queue_multiplayer_action(const char* action, const std::string& argument) {
    const auto callback = private_queue.load();
    std::array<char, 256> result{};
    return callback && callback(action, argument.c_str(), "", result.data(), result.size());
}
bool queue_multiplayer_action(const char* action, const std::string& argument, const std::string& password) {
    const auto callback = private_queue.load();
    std::array<char, 256> result{};
    return callback && callback(action, argument.c_str(), password.c_str(), result.data(), result.size());
}
} // namespace dingosdk::overlay::detail

namespace dingosdk::overlay::menu::multiplayer_detail {
bool send_private(SkateMenu &menu, const char *action, const std::string &argument,
                  std::array<char, 65> &password, bool use_password) {
    const auto callback = private_queue.load();
    std::array<char, 512> result{};
    if (!callback) {
        feedback(menu, "Multiplayer dispatcher unavailable.");
        return false;
    }
    const bool accepted =
        callback(action, argument.c_str(), use_password ? password.data() : "", result.data(), result.size());
    result.back() = 0;
    feedback(menu, result.data());
    if (accepted)
        SecureZeroMemory(password.data(), password.size());
    return accepted;
}
std::string map_label(const Model &model, std::string_view path) {
    return world_destination_name(path, model.levels);
}
} // namespace dingosdk::overlay::menu::multiplayer_detail

namespace dingosdk::overlay::menu {
using namespace multiplayer_detail;
namespace {
void host_page(SkateMenu &menu, const Model &model, const CallbacksV3 &) {
    const auto &mp = model.multiplayer;
    if (!menu.multiplayer_host_seeded && mp.saved_host.loaded) {
        // Start from the settings used last time (the password is never saved).
        const auto &saved = mp.saved_host;
        menu.multiplayer_host_seeded = true;
        menu.multiplayer_visibility = saved.public_lobby ? 1 : 0;
        menu.multiplayer_capacity = saved.capacity;
        menu.multiplayer_tps = saved.tps;
        menu.multiplayer_password_enabled = saved.password_required;
        menu.multiplayer_lobby_name.fill(0);
        std::copy_n(saved.lobby_name.begin(), std::min(saved.lobby_name.size(), menu.multiplayer_lobby_name.size() - 1),
                    menu.multiplayer_lobby_name.begin());
    }
    const bool editable = !mp.active && !mp.lobby_joining;
    int visibility = mp.hosting ? (mp.public_host ? 1 : 0) : menu.multiplayer_visibility;
    int capacity = mp.hosting ? mp.capacity : menu.multiplayer_capacity;
    unsigned tps = mp.hosting ? mp.tps : menu.multiplayer_tps;
    bool locked = mp.hosting ? mp.password_required : menu.multiplayer_password_enabled;

    // Who can get in, and the session's size and speed: two cards of about one height, so in a wide
    // window they sit side by side.
    begin_card(menu, "host-access", "LOBBY");
    info(menu, "Map", map_label(model, mp.map));
    {
        static const std::vector<std::string> visibilities{"Join code only", "Public lobby"};
        option_row(menu, "Who can join", "Whether the lobby is listed in Servers or only found with your join code.",
                   visibility, visibilities, editable);
    }
    menu.multiplayer_visibility = visibility;
    note(visibility == 0 ? "Unlisted: friends join with the code you share from Session." : "Listed in Servers: anyone on a compatible build can join.");
    ImGui::BeginDisabled(!editable);
    field(menu, "Lobby name");
    const auto name_hint = mp.local_name.empty() ? std::string("Your Steam name") : mp.local_name + " (your Steam name)";
    ImGui::InputTextWithHint("##lobby-name", name_hint.c_str(), menu.multiplayer_lobby_name.data(),
                             menu.multiplayer_lobby_name.size());
    ImGui::EndDisabled();
    toggle_row(menu, "Require password", "Players must enter a password to join.", locked, editable, locked ? "ON" : "OFF");
    menu.multiplayer_password_enabled = locked;
    if (locked && !mp.hosting) {
        field(menu, "Password");
        ImGui::InputTextWithHint("##host-password", "Choose a lobby password", menu.multiplayer_host_password.data(),
                                 menu.multiplayer_host_password.size(), ImGuiInputTextFlags_Password);
    }
    end_card();

    begin_card(menu, "host-settings", "PLAYERS AND SPEED");
    ImGui::BeginDisabled(!editable);
    field(menu, "Player limit", "Includes you.");
    ::dingosdk::overlay::menu::slider_int("##player-limit", &capacity, 2, multiplayer_lobby_player_limit, "%d players");
    menu.multiplayer_capacity = capacity;
    ImGui::EndDisabled();
    {
        // The tick rates offered, one per step of left / right.
        std::vector<std::string> rates;
        int rate = 0;
        for (const auto offered : multiplayer_tick_rates) {
            if (offered == tps) rate = static_cast<int>(rates.size());
            rates.push_back(std::to_string(offered) + " TPS");
        }
        if (option_row(menu, "Update rate", "How often nearby skaters update. Higher is smoother but uses more upload bandwidth.", rate, rates,
                       editable))
            tps = multiplayer_tick_rates[static_cast<std::size_t>(rate)];
    }
    menu.multiplayer_tps = tps;
    const auto rates = "Nearby players update at " + std::to_string(tps) + " TPS, distant ones at 10 and 5.";
    note(rates.c_str());
    end_card();

    if (!mp.hosting) {
        if (primary_button(menu, visibility ? "HOST PUBLIC LOBBY" : "HOST WITH JOIN CODE",
                           editable && mp.local_ready)) {
            if (locked && !menu.multiplayer_host_password[0])
                feedback(menu, "Enter a password or turn off Require password.");
            else
                send_private(menu, "host-config",
                             std::string(visibility ? "public " : "code ") + std::to_string(capacity) + " " +
                                 std::to_string(tps) + " " + menu.multiplayer_lobby_name.data(),
                             menu.multiplayer_host_password, locked);
        }
        if (!mp.local_ready) warn("Load a map before hosting.");
        else if (mp.active) warn("Leave your current session before hosting.");
    }
    if (!mp.lobby_status.empty()) note(mp.lobby_status.c_str());
}
} // namespace
// Local-only display preferences; they live in Settings > Interface.
void multiplayer_display_settings(SkateMenu &menu, const Model &model) {
    const auto &mp = model.multiplayer;
    // Three cards by topic, so in a wide window they share the two columns.
    begin_card(menu, "multiplayer-players", "PLAYERS");
    {
        std::array<char, 65> unused{};
        field(menu, "Player distance", "How far away other players are still shown as skaters. Past it they keep their nametag "
                                       "or dot. Lower it on a busy server for more frames and less memory; all the way up shows everyone.");
        float shown = menu.player_distance_pending.value_or(mp.player_distance);
        ::dingosdk::overlay::menu::slider_float("##player-distance", &shown, player_distance_least, player_distance_unlimited,
                           shown >= player_distance_unlimited ? "Everyone" : "%.0f m", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) menu.player_distance_pending = shown;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            send_private(menu, "player-distance", std::to_string(static_cast<int>(shown)), unused, false);
            menu.player_distance_until = ImGui::GetTime() + 2;
        }
        if (!ImGui::IsItemActive() && menu.player_distance_pending &&
            (*menu.player_distance_pending == mp.player_distance || ImGui::GetTime() >= menu.player_distance_until))
            menu.player_distance_pending.reset();
    }
    bool direct = mp.prefer_direct;
    if (toggle_row(menu, "Direct connections",
                   "Connect straight to dedicated servers that offer it: the shortest route, so the lowest ping. The server can then "
                   "see your IP address, as with any game's dedicated servers; other players never can. Off: always through Steam's "
                   "relays. Applies from the next server you join.",
                   direct)) {
        std::array<char, 65> unused{};
        send_private(menu, "direct-connections", direct ? "on" : "off", unused, false);
    }
    if (dingosdk::discord_presence::available()) {
        bool discord = dingosdk::discord_presence::enabled();
        if (toggle_row(menu, "Discord status",
                "Show on your Discord profile where you are skating: the map, the server or lobby and how many are in it. A session with a password shows no name.",
                discord))
            dingosdk::discord_presence::set_enabled(discord);
    }
    end_card();

    begin_card(menu, "multiplayer-nametags", "NAMETAGS");
    bool nametags = mp.nametags;
    if (toggle_row(menu, "Player nametags",
                   "The name above each skater, with their distance: purple for ReSkate developers, red for content creators, "
                   "gold for homies, pink for server admins, blue for the host, green for your Steam friends.",
                   nametags)) {
        std::array<char, 65> unused{};
        send_private(menu, "nametags", nametags ? "on" : "off", unused, false);
    }
    if (mp.nametags) {
        std::array<char, 65> unused{};
        field(menu, "Nametag distance", "How far away a player's name still shows. Past it they are a dot.");
        float distance = menu.nametag_distance_pending.value_or(mp.nametag_distance);
        ::dingosdk::overlay::menu::slider_float("##nametag-distance", &distance, 10.f, 500.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) menu.nametag_distance_pending = distance;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            send_private(menu, "nametag-distance", std::to_string(static_cast<int>(distance)), unused, false);
            menu.nametag_distance_until = ImGui::GetTime() + 2;
        }
        if (!ImGui::IsItemActive() && menu.nametag_distance_pending &&
            (*menu.nametag_distance_pending == mp.nametag_distance || ImGui::GetTime() >= menu.nametag_distance_until))
            menu.nametag_distance_pending.reset();
    }
    bool dots = mp.nametag_dots;
    if (toggle_row(menu, "Nametag dots", "Show players past the nametag distance, and players off screen, as dots.", dots,
                   mp.nametags, "OFF")) {
        std::array<char, 65> unused{};
        send_private(menu, "nametag-dots", dots ? "on" : "off", unused, false);
    }
    bool friends_only = mp.nametags_friends;
    if (toggle_row(menu, "Friends' nametags only", "Only your Steam friends have a name or dot. Chat bubbles still show for everyone.",
                   friends_only, mp.nametags, "OFF")) {
        std::array<char, 65> unused{};
        send_private(menu, "nametags-friends", friends_only ? "on" : "off", unused, false);
    }
    end_card();

    begin_card(menu, "multiplayer-chat", "CHAT");
    bool chat = mp.chat_visible;
    if (toggle_row(menu, "Text chat", "Show session chat in the bottom-right corner; T opens it. Hidden, nothing shows and T does nothing.",
            chat)) {
        std::array<char, 65> unused{};
        send_private(menu, "chat-visible", chat ? "on" : "off", unused, false);
    }
    bool filter = mp.chat_filter;
    if (toggle_row(menu, "Chat filter", "Show bad words in chat messages and names as ****.", filter, mp.chat_visible, "OFF")) {
        std::array<char, 65> unused{};
        send_private(menu, "chat-filter", filter ? "on" : "off", unused, false);
    }
    bool bubbles = mp.chat_bubbles;
    if (toggle_row(menu, "Chat bubbles", "Show each player's newest chat line in a bubble above their skater.", bubbles)) {
        std::array<char, 65> unused{};
        send_private(menu, "chat-bubbles", bubbles ? "on" : "off", unused, false);
    }
    bool own_bubbles = mp.chat_bubbles_own;
    if (toggle_row(menu, "Own chat bubbles", "Also show your own messages above your skater.", own_bubbles, mp.chat_bubbles,
                   "OFF")) {
        std::array<char, 65> unused{};
        send_private(menu, "chat-bubbles-own", own_bubbles ? "on" : "off", unused, false);
    }
    if (mp.chat_bubbles) {
        std::array<char, 65> unused{};
        field(menu, "Bubble distance", "How far away a player can be and still show a chat bubble.");
        float distance = menu.chat_bubbles_distance_pending.value_or(mp.chat_bubbles_distance);
        ::dingosdk::overlay::menu::slider_float("##bubble-distance", &distance, 5.f, 200.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) menu.chat_bubbles_distance_pending = distance;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            send_private(menu, "chat-bubbles-distance", std::to_string(static_cast<int>(distance)), unused, false);
            menu.chat_bubbles_distance_until = ImGui::GetTime() + 2;
        }
        if (!ImGui::IsItemActive() && menu.chat_bubbles_distance_pending &&
            (*menu.chat_bubbles_distance_pending == mp.chat_bubbles_distance ||
             ImGui::GetTime() >= menu.chat_bubbles_distance_until))
            menu.chat_bubbles_distance_pending.reset();
        field(menu, "Bubble duration", "How many seconds a chat bubble stays before it fades.");
        float duration = menu.chat_bubbles_duration_pending.value_or(mp.chat_bubbles_duration);
        ::dingosdk::overlay::menu::slider_float("##bubble-duration", &duration, 1.f, 30.f, "%.0f s", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) menu.chat_bubbles_duration_pending = duration;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            send_private(menu, "chat-bubbles-duration", std::to_string(static_cast<int>(duration)), unused, false);
            menu.chat_bubbles_duration_until = ImGui::GetTime() + 2;
        }
        if (!ImGui::IsItemActive() && menu.chat_bubbles_duration_pending &&
            (*menu.chat_bubbles_duration_pending == mp.chat_bubbles_duration ||
             ImGui::GetTime() >= menu.chat_bubbles_duration_until))
            menu.chat_bubbles_duration_pending.reset();
        field(menu, "Bubble history", "How many recent messages stack above each skater (1-8).");
        int history = menu.chat_bubbles_history_pending.value_or(mp.chat_bubbles_history);
        ::dingosdk::overlay::menu::slider_int("##bubble-history", &history, 1, 8, "%d lines", ImGuiSliderFlags_AlwaysClamp);
        if (ImGui::IsItemActive()) menu.chat_bubbles_history_pending = history;
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            send_private(menu, "chat-bubbles-history", std::to_string(history), unused, false);
            menu.chat_bubbles_history_until = ImGui::GetTime() + 2;
        }
        if (!ImGui::IsItemActive() && menu.chat_bubbles_history_pending &&
            (*menu.chat_bubbles_history_pending == mp.chat_bubbles_history ||
             ImGui::GetTime() >= menu.chat_bubbles_history_until))
            menu.chat_bubbles_history_pending.reset();
    }
    end_card();
    note("Nametags and chat only change your screen; nobody else is affected.");
}
void multiplayer_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks) {
    const auto &mp = model.multiplayer;
    const bool has_session = mp.active || mp.lobby_joining;
    if (has_session && !menu.multiplayer_session_seen) menu.multiplayer_tab = multiplayer_session_tab;
    menu.multiplayer_session_seen = has_session;
    // A player's tag and item colours are on the Skater page's STYLE tab (skate_menu_skater.cpp).
    menu.multiplayer_tab = std::clamp(menu.multiplayer_tab, 0, multiplayer_chat_tab);
    category_tabs(menu, menu.multiplayer_tab, {"SERVERS", has_session ? "SESSION" : "HOST", "VOICE", "BANS AND IGNORED", "PLAYERS AND CHAT"},
                  "multiplayer-categories");
    if (menu.multiplayer_tab != multiplayer_voice_tab) menu.voice_bind_capture = 0;
    ImGui::PushID(menu.multiplayer_tab);
    ImGui::BeginChild("multiplayer-category", ImVec2(0, page_body_height(menu)), ImGuiChildFlags_NavFlattened);
    switch (menu.multiplayer_tab) {
    case 0:
        join_page(menu, model, callbacks);
        break;
    case multiplayer_session_tab:
        // Hosting or joining turns the host form into the session it started.
        if (has_session) session_page(menu, model);
        else host_page(menu, model, callbacks);
        break;
    case multiplayer_voice_tab:
        voice_controls(menu, mp);
        break;
    case multiplayer_bans_tab:
        bans_page(menu, model);
        break;
    default:
        menu::multiplayer_display_settings(menu, model);
        break;
    }
    ImGui::EndChild();
    ImGui::PopID();
    password_popup(menu, model);
    ban_popup(menu);
}
} // namespace dingosdk::overlay::menu
