#include "multiplayer_menu_internal.h"
#include "role_badge.h"
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
void cell_text(const std::string &text) {
    const float width = std::max(1.f, ImGui::GetContentRegionAvail().x);
    const char *first = text.data(), *last = first + text.size();
    const bool clipped = ImGui::CalcTextSize(first, last).x > width;
    if (clipped) {
        const float dots = ImGui::CalcTextSize("...").x;
        while (last > first && ImGui::CalcTextSize(first, last).x + dots > width) {
            --last;
            while (last > first && (static_cast<unsigned char>(*last) & 0xc0) == 0x80)
                --last;
        }
        ImGui::Text("%.*s...", static_cast<int>(last - first), first);
    } else
        ImGui::TextUnformatted(first, last);
    if (clipped && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text.c_str());
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

    begin_card(menu, "host-access", "WHO CAN JOIN");
    choice(menu, "visibility", visibility, {"Join code only", "Public lobby"}, editable);
    menu.multiplayer_visibility = visibility;
    note(visibility == 0 ? "Unlisted. Friends join with the code you share from Session."
                         : "Listed in the Servers browser. Anyone on a compatible build can join.");
    end_card();

    begin_card(menu, "host-settings", "LOBBY SETTINGS");
    info(menu, "Map", map_label(model, mp.map));
    ImGui::BeginDisabled(!editable);
    field(menu, "Lobby name");
    const auto name_hint = mp.local_name.empty() ? std::string("Your Steam name") : mp.local_name + " (your Steam name)";
    ImGui::InputTextWithHint("##lobby-name", name_hint.c_str(), menu.multiplayer_lobby_name.data(),
                             menu.multiplayer_lobby_name.size());
    field(menu, "Player limit", "Includes you.");
    ImGui::SliderInt("##player-limit", &capacity, 2, multiplayer_lobby_player_limit, "%d players");
    menu.multiplayer_capacity = capacity;
    field(menu, "Update rate",
          "How often nearby skaters update. Higher is smoother but uses more upload bandwidth.");
    if (ImGui::BeginCombo("##tps", (std::to_string(tps) + " TPS").c_str())) {
        for (const auto rate : multiplayer_tick_rates) {
            if (ImGui::Selectable((std::to_string(rate) + " TPS").c_str(), rate == tps)) tps = rate;
            if (rate == tps) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    menu.multiplayer_tps = tps;
    ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, px(2)));
    toggle_row(menu, "Require password", "Players must enter a password to join.", locked, editable, locked ? "ON" : "OFF");
    menu.multiplayer_password_enabled = locked;
    if (locked && !mp.hosting) {
        field(menu, "Password");
        ImGui::InputTextWithHint("##host-password", "Choose a lobby password", menu.multiplayer_host_password.data(),
                                 menu.multiplayer_host_password.size(), ImGuiInputTextFlags_Password);
    }
    const auto rates = "Nearby players update at " + std::to_string(tps) + " TPS; distant players at 10 and 5 TPS.";
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
std::string number(unsigned long long value) { return std::to_string(value); }
std::string decimal(double value, int digits = 1) {
    std::array<char, 64> text{};
    std::snprintf(text.data(), text.size(), "%.*f", digits, value);
    return text.data();
}
void debug_page(SkateMenu &menu, const MultiplayerModel &mp, const CallbacksV3 &callbacks) {
    begin_card(menu, "local-test", "LOCAL TEST");
    note("Local Echo replays your skater, board, cosmetics and sound with a short delay, on this PC only.");
    ImGui::BeginDisabled(mp.active || mp.lobby_joining || !mp.local_ready);
    if (ImGui::Button("Start Local Echo", ImVec2(-FLT_MIN, 0)))
        send_console(menu, callbacks, "mp echo");
    ImGui::EndDisabled();
    const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
    if (ImGui::Button("Test Steam transport", ImVec2(half, 0)))
        send_console(menu, callbacks, "mp test");
    ImGui::SameLine();
    if (ImGui::Button("Retry remote skater", ImVec2(-FLT_MIN, 0)))
        send_console(menu, callbacks, "mp retry");
    for (const auto *detail : {&mp.native_status, &mp.player_ui_status, &mp.cosmetic_status, &mp.audio_status})
        if (!detail->empty()) note(detail->c_str());
    end_card();

    begin_card(menu, "connection", "CONNECTION");
    info(menu, "Steam ID", number(mp.local_id));
    if (mp.peer_id) info(menu, "Peer", number(mp.peer_id) + (mp.peer_name.empty() ? "" : "  (" + mp.peer_name + ")"));
    info(menu, "Update rate", std::to_string(mp.tps) + " TPS nearby, 10 / 5 TPS at distance");
    info(menu, "Routes", std::to_string(mp.direct_connections) + " direct, " + std::to_string(mp.fallback_streams) +
                             " forwarded by the host");
    if (mp.active && !mp.hosting && !mp.echo)
        info(menu, "Direct upload limit", std::to_string(mp.direct_upload_limit) + " peers (automatic)");
    info(menu, "Messages", number(mp.sent) + " sent, " + number(mp.received) + " received");
    if (mp.network_telemetry) {
        info(menu, "Ping", std::to_string(mp.ping_ms) + " ms");
        info(menu, "Traffic", decimal(mp.outgoing_bps / 1024.f) + " KiB/s out, " + decimal(mp.incoming_bps / 1024.f) + " KiB/s in");
        info(menu, "Send budget", decimal(mp.send_rate / 1024.f) + " KiB/s total");
        info(menu, "Gameplay queue", decimal(static_cast<double>(mp.queue_us) / 1000.0) + " ms (" +
                                         std::to_string(mp.pending_bytes) + " bytes)");
        info(menu, "Cosmetic queue", decimal(static_cast<double>(mp.cosmetic_queue_us) / 1000.0) + " ms, " +
                                         std::to_string(mp.prioritized_connections) + " prioritized");
        if (mp.delivery_local >= 0 && mp.delivery_remote >= 0)
            info(menu, "In-order delivery", decimal(mp.delivery_local * 100.f) + "% here, " +
                                                decimal(mp.delivery_remote * 100.f) + "% there");
    }
    info(menu, "Skipped / errors", number(mp.skipped_updates) + " stale skipped, " + number(mp.send_failures) +
                                       " send errors, " + number(mp.invalid_messages) + " invalid");
    if (mp.raw_sent_bytes)
        info(menu, "Compression", decimal(100.0 * (1.0 - static_cast<double>(mp.sent_bytes) /
                                                        static_cast<double>(mp.raw_sent_bytes))) + "% of outgoing bytes saved");
    note("Automatic LZ4 / Zstd compression; unchanged bone fields are left out.");
    end_card();

    begin_card(menu, "animation", "SKATERS AND SOUND");
    info(menu, "Captured bones", std::to_string(mp.skater_bones) + " skater, " + std::to_string(mp.board_bones) + " board");
    info(menu, "Remote updates", number(mp.pose_updates) + " skater, " + number(mp.board_pose_updates) + " board");
    info(menu, "Player map updates", number(mp.player_map_updates));
    info(menu, "Sound frames", number(mp.audio_captured) + " captured, " + number(mp.audio_played) + " played");
    end_card();

    if (mp.active) {
        begin_card(menu, "timing", "CLIENT TIMING", "average / peak per callback, 5 s window");
        const auto &timing = mp.client_timing;
        info(menu, "Callbacks", decimal(timing.callback_hz) + "/s, longest gap " + decimal(timing.gap_max_ms, 2) + " ms");
        info(menu, "Multiplayer work", decimal(timing.work_ms, 2) + " / " + decimal(timing.work_max_ms, 2) + " ms");
        constexpr std::array labels{"Capture / setup", "Receive / routing", "Encode / send", "Remote playback", "Diagnostics"};
        for (unsigned i = 0; i < labels.size(); ++i)
            info(menu, labels[i], decimal(timing.mean_ms[i], 2) + " / " + decimal(timing.peak_ms[i], 2) + " ms");
        note("Native animation work runs in engine hooks outside these phases.");
        end_card();
    }

    if (!mp.roster.empty()) {
        begin_card(menu, "peers", "PEERS");
        for (const auto &peer : mp.roster) {
            const auto label = peer.name + "##peer-" + std::to_string(peer.id);
            if (!ImGui::TreeNode(label.c_str())) continue;
            info(menu, "Route", peer.route + ", " + decimal(peer.pose_hz) + " poses/s, target " +
                                    std::to_string(peer.pose_target_tps) + " TPS");
            if (peer.distance_m >= 0) info(menu, "Distance", decimal(peer.distance_m, 0) + " m");
            info(menu, "Newest pose", number(peer.pose_age_ms) + " ms ago");
            info(menu, "Native updates", decimal(peer.native_pose_hz) + " skater, " + decimal(peer.native_board_hz) + " board /s");
            info(menu, "Remote graph", decimal(peer.native_animation_hz) + "/s at " + decimal(peer.native_animation_ms, 3) +
                                           " ms, pose apply " + decimal(peer.pose_apply_ms, 3) + " ms");
            info(menu, "Graph skips", number(peer.native_animation_skipped));
            info(menu, "Playback", peer.playback + ", prediction " + std::to_string(peer.prediction_ms) + " ms" +
                                       (peer.correcting ? ", correcting" : ""));
            for (const auto *detail : {&peer.native_status, &peer.cosmetic_status, &peer.audio_status, &peer.ui_status})
                if (!detail->empty()) note(detail->c_str());
            ImGui::TreePop();
        }
        end_card();
    }
}
} // namespace
// Local-only display preferences; they live in Settings > Interface.
void multiplayer_display_settings(SkateMenu &menu, const Model &model) {
    const auto &mp = model.multiplayer;
    begin_card(menu, "multiplayer-display", "MULTIPLAYER");
    {
        std::array<char, 65> unused{};
        field(menu, "Player distance", "How far away other players are still shown as skaters. Past it they keep their nametag "
                                       "or dot. Lower it on a busy server for more frames and less memory; all the way up shows everyone.");
        float shown = menu.player_distance_pending.value_or(mp.player_distance);
        ImGui::SliderFloat("##player-distance", &shown, player_distance_least, player_distance_unlimited,
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
        ImGui::SliderFloat("##nametag-distance", &distance, 10.f, 500.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
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
    if (dingosdk::discord_presence::available()) {
        bool discord = dingosdk::discord_presence::enabled();
        if (toggle_row(menu, "Discord status",
                "Show on your Discord profile where you are skating: the map, the server or lobby and how many are in it. A session with a password shows no name.",
                discord))
            dingosdk::discord_presence::set_enabled(discord);
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
        ImGui::SliderFloat("##bubble-distance", &distance, 5.f, 200.f, "%.0f m", ImGuiSliderFlags_AlwaysClamp);
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
        ImGui::SliderFloat("##bubble-duration", &duration, 1.f, 30.f, "%.0f s", ImGuiSliderFlags_AlwaysClamp);
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
        ImGui::SliderInt("##bubble-history", &history, 1, 8, "%d lines", ImGuiSliderFlags_AlwaysClamp);
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
    note("These only change your screen; nobody else is affected.");
    end_card();
}
void multiplayer_network_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks) {
    debug_page(menu, model.multiplayer, callbacks);
}
// SPECIAL: the page only a player on one of the backend's lists gets. Their tag as everyone
// sees it, and whether it shows; then the items that come with it, on or off together and
// each on its own: what their list gives it, two colours of their own, or nothing.
void special_page(SkateMenu &menu, const Model &model, const CallbacksV3 &) {
    const auto &mp = model.multiplayer;
    std::array<char, 65> unused{};
    ImGui::BeginChild("special", ImVec2(0, page_body_height(menu)));
    begin_card(menu, "special-tag", "YOUR TAG");
    {
        // Drawn the way chat and nametags draw it (role_badge.h), and faint while it is off.
        field(menu, "Looks like", "Your tag and name, as chat and your nametag show them.");
        const std::string name = mp.local_name.empty() ? std::string("You") : mp.local_name;
        const float size = px(18), height = ImGui::GetFrameHeight(), alpha = mp.identity_tag_shown ? 1.0f : .3f;
        auto *draw = ImGui::GetWindowDrawList();
        const auto at = ImGui::GetCursorScreenPos();
        const float badge = overlay::detail::role_badge_width(menu.bold, size, mp.identity_tag) + px(6);
        overlay::detail::draw_role_badge(draw, menu.bold, size, at, height, mp.identity_tag, mp.identity_tag_colour, alpha);
        const auto extent = menu.bold->CalcTextSizeA(size, FLT_MAX, 0, name.c_str());
        const ImVec2 name_at(at.x + badge, at.y + (height - extent.y) * .5f);
        const int name_vertices = draw->VtxBuffer.Size;
        draw->PushClipRect(at, ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y + height), true);
        draw->AddText(menu.bold, size, name_at,
                      (mp.identity_tag_colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(255.0f * alpha) << IM_COL32_A_SHIFT), name.c_str());
        draw->PopClipRect();
        overlay::detail::shade_nametag_gradient(draw, name_vertices, name_at.x, extent.x, mp.identity_tag_colour, ImGui::GetTime());
        ImGui::Dummy(ImVec2(std::min(badge + extent.x, ImGui::GetContentRegionAvail().x), height));
    }
    bool tag = mp.identity_tag_shown;
    if (toggle_row(menu, "Show my tag",
                   "Your tag and the color of your name, in chat and on your nametag. "
                   "Off: your name looks like any other player's, to yourself and to everyone you skate with.",
                   tag))
        send_private(menu, "mark-tag", tag ? "on" : "off", unused, false);
    end_card();

    begin_card(menu, "special-items", "YOUR ITEMS");
    bool shown = mp.identity_items_shown;
    if (toggle_row(menu, "Show my items",
                   "The colors that come with your tag, on whatever you wear and ride. "
                   "Off: it all keeps its own colors, for you and for everyone you skate with.",
                   shown))
        send_private(menu, "mark-items", shown ? "on" : "off", unused, false);
    note("Below, each one on its own: it colors whatever you have on in that slot. Everyone you skate with sees what you pick.");
    end_card();

    // The colours being picked: held here while the mouse has them, and after it lets go until
    // the saved ones catch up, so a swatch never jumps back to the colour it had.
    struct Picking {
        std::array<float, 3> from{}, to{};
        bool changed{};
        int waiting{}; // frames
    };
    static std::vector<Picking> picking;
    picking.resize(mp.identity_styles.size());
    for (int item = 0; item < static_cast<int>(mp.identity_styles.size()); ++item) {
        const auto &style = mp.identity_styles[static_cast<std::size_t>(item)];
        auto &picked = picking[static_cast<std::size_t>(item)];
        const auto same = [](const std::array<float, 3> &a, const std::array<float, 3> &b) {
            return std::abs(a[0] - b[0]) < .003f && std::abs(a[1] - b[1]) < .003f && std::abs(a[2] - b[2]) < .003f;
        };
        if (picked.waiting && same(picked.from, style.from) && same(picked.to, style.to)) picked.waiting = 0;
        else if (picked.waiting) --picked.waiting;
        if (!picked.changed && !picked.waiting) picked.from = style.from, picked.to = style.to;
        const auto send = [&](int mode, int speed) {
            const auto hex = [](const std::array<float, 3> &colour) {
                unsigned value{};
                for (const float part : colour) value = value << 8 | static_cast<unsigned>(std::clamp(part, 0.0f, 1.0f) * 255.0f + 0.5f);
                return value;
            };
            send_private(menu, "mark-style", std::format("{} {} {:06x} {:06x} {}", item, mode, hex(picked.from), hex(picked.to), speed),
                         unused, false);
        };
        // The card is headed by the cosmetic: whatever is in that slot is what gets coloured.
        std::string title = style.name;
        for (auto &letter : title) letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
        ImGui::PushID(item);
        begin_card(menu, title.c_str(), title.c_str());
        // The options in the order shown; a style's mode is 0 what the list gives, 1 off, 2 a
        // gradient between the player's two colours, 3 their one colour, 4 the rainbow (the
        // staff's to pick).
        field(menu, "Color");
        if (mp.identity_rainbow) {
            static constexpr std::array<int, 5> modes{0, 4, 2, 3, 1};
            int option = static_cast<int>(std::find(modes.begin(), modes.end(), style.mode) - modes.begin()) % 5;
            if (choice(menu, "mode", option, {mp.identity_animation.c_str(), "RAINBOW", "GRADIENT", "SOLID", "OFF"}, shown))
                send(modes[static_cast<std::size_t>(option)], style.speed);
        } else {
            static constexpr std::array<int, 4> modes{0, 2, 3, 1};
            int option = static_cast<int>(std::find(modes.begin(), modes.end(), style.mode) - modes.begin()) % 4;
            if (choice(menu, "mode", option, {mp.identity_animation.c_str(), "GRADIENT", "SOLID", "OFF"}, shown))
                send(modes[static_cast<std::size_t>(option)], style.speed);
        }
        if (style.mode == 2 || style.mode == 3) {
            const bool gradient = style.mode == 2;
            field(menu, gradient ? "Colors" : "Pick", gradient ? "The two colors it moves between. Click one to change it."
                                                               : "Click the color to change it.");
            ImGui::BeginDisabled(!shown);
            constexpr auto flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoAlpha;
            if (ImGui::ColorEdit3("##from", picked.from.data(), flags)) picked.changed = true;
            // Beside the swatch, what it gives: the one colour, or all that lies between the two.
            ImGui::SameLine(0, px(6));
            const auto at = ImGui::GetCursorScreenPos();
            const float swatch = gradient ? ImGui::GetFrameHeight() + px(6) : 0.0f;
            const ImVec2 bar(std::max(px(20), ImGui::GetContentRegionAvail().x - swatch), ImGui::GetFrameHeight());
            const auto solid = [&](const std::array<float, 3> &colour) {
                return ImGui::ColorConvertFloat4ToU32(ImVec4(colour[0], colour[1], colour[2], shown ? 1.0f : .4f));
            };
            const auto left = solid(picked.from), right = solid(gradient ? picked.to : picked.from);
            ImGui::GetWindowDrawList()->AddRectFilledMultiColor(at, ImVec2(at.x + bar.x, at.y + bar.y), left, right, right, left);
            ImGui::Dummy(bar);
            if (gradient) {
                ImGui::SameLine(0, px(6));
                if (ImGui::ColorEdit3("##to", picked.to.data(), flags)) picked.changed = true;
            }
            ImGui::EndDisabled();
            // One save when the mouse lets go, not one for every shade dragged through.
            if (picked.changed && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                picked.changed = false;
                picked.waiting = 120;
                send(style.mode, style.speed);
            }
        }
        // A colour that stands still has no speed.
        if (style.mode == 0 || style.mode == 2 || style.mode == 4) {
            int speed = style.speed;
            field(menu, "Speed");
            if (choice(menu, "speed", speed, {"NORMAL", "SLOW", "FAST"}, shown)) send(style.mode, speed);
        }
        end_card();
        ImGui::PopID();
    }
    ImGui::EndChild();
}
void multiplayer_page(SkateMenu &menu, const Model &model, const CallbacksV3 &callbacks) {
    const auto &mp = model.multiplayer;
    const bool has_session = mp.active || mp.lobby_joining;
    if (has_session && !menu.multiplayer_session_seen) menu.multiplayer_tab = multiplayer_session_tab;
    menu.multiplayer_session_seen = has_session;
    category_tabs(menu, menu.multiplayer_tab, {"SERVERS", has_session ? "SESSION" : "HOST", "VOICE", "BANS"},
                  "multiplayer-categories");
    if (menu.multiplayer_tab != multiplayer_voice_tab) menu.voice_bind_capture = 0;
    ImGui::PushID(menu.multiplayer_tab);
    ImGui::BeginChild("multiplayer-category", ImVec2(0, page_body_height(menu)));
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
    }
    ImGui::EndChild();
    ImGui::PopID();
    password_popup(menu, model);
    ban_popup(menu);
}
} // namespace dingosdk::overlay::menu
