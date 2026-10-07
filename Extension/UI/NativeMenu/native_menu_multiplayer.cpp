#include "native_menu_internal.h"
#include "Extension/Multiplayer/Session/session.h"
#include "Engine/Game/Input/voice_input.h"
#include <Windows.h>
#include <algorithm>

namespace dingosdk::multiplayer {
using namespace menu_data;
using namespace native_menu_detail;
namespace {
bool copy_code_to_clipboard(const std::string& code) noexcept {
    if (code.empty()) return false;
    const auto window = GetForegroundWindow();
    DWORD process{};
    if (!window || !GetWindowThreadProcessId(window, &process) || process != GetCurrentProcessId()) return false;
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, (code.size() + 1) * sizeof(wchar_t));
    if (!memory) return false;
    auto* text = static_cast<wchar_t*>(GlobalLock(memory));
    if (!text) { GlobalFree(memory); return false; }
    // Join codes use ASCII, stored as Unicode text for other Windows apps.
    std::copy(code.begin(), code.end(), text);
    text[code.size()] = L'\0';
    GlobalUnlock(memory);
    if (!OpenClipboard(window)) { GlobalFree(memory); return false; }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!copied) GlobalFree(memory); // Windows owns the allocation after success.
    return copied;
}
// Voice buttons cycle through these steps, wrapping after the last.
constexpr std::array voice_distances{10.f, 20.f, 35.f, 50.f, 75.f, 100.f, 150.f, 300.f};
constexpr std::array voice_volumes{0.f, .25f, .5f, .75f, 1.f, 1.5f, 2.f, 3.f, 5.f};
template<std::size_t N> float next_step(float current, const std::array<float, N>& steps) {
    const auto found = std::upper_bound(steps.begin(), steps.end(), current + 0.001f);
    return found == steps.end() ? steps.front() : *found;
}
std::string volume_label(float value) {
    auto text = std::to_string(value);
    return text.substr(0, text.find('.') + 3) + "x";
}
} // namespace
namespace native_menu_detail {
void process_clipboard() noexcept {
    auto& s = state();
    if (!s.copy_code_requested) return;
    s.copy_code_requested = false;
    try {
        const auto model = multiplayer::model();
        s.feedback = !model.active || model.invite.empty() ? "No active join code to copy." :
            copy_code_to_clipboard(model.invite) ? "Join code copied." : "Couldn't copy the join code. Try again.";
        s.feedback_status = model.status;
        s.feedback_until = GetTickCount64() + 4000;
        s.next_render = 0;
    } catch (...) { }
}
void process_actions(const Context& context, const MultiplayerModel& model) {
    auto& s = state();
    if (!s.host_seeded && model.saved_host.loaded) {
        // Start from the host settings used last time.
        s.host_seeded = true;
        s.public_lobby = model.saved_host.public_lobby;
        s.capacity = static_cast<unsigned>(model.saved_host.capacity);
        s.tps = model.saved_host.tps;
    }
    std::deque<Action> requests;
    { std::lock_guard lock(s.mutex); requests.swap(s.pending); }
    for (const auto& request : requests) {
        if (request.generation != s.generation.load()) continue;
        s.feedback_until = 0;
        const auto& cmd = request.command;
        if (cmd == "section") {
            s.section = request.argument == "join" ? Section::join : request.argument == "session" ? Section::session :
                request.argument == "host" ? Section::host : Section::browser;
            context.set(context.path(s.page, {content, 0x18f8355b}), static_cast<int>(s.section));
            s.feedback.clear();
        } else if (cmd == "filter-map") {
            s.browser.same_map = !s.browser.same_map;
        } else if (cmd == "sort") {
            s.browser.sort = static_cast<menu_view::Sort>((static_cast<unsigned>(s.browser.sort) + 1) % 3);
        } else if (cmd == "select-player") {
            s.selected_player = menu_view::selected_player(model, request.argument) ? request.argument : "";
            s.feedback.clear();
        } else if (cmd == "session-details") {
            s.selected_player.clear();
            s.feedback.clear();
        } else if (cmd == "copy-code") {
            s.copy_code_requested = true;
        } else if (cmd == "code-mode") {
            s.protected_lobby.clear(); clear_input(context, "join-password");
        } else if (cmd == "visibility") {
            if (!model.active && !model.lobby_joining) s.public_lobby = !s.public_lobby;
        } else if (cmd == "capacity") {
            if (!model.active && !model.lobby_joining) {
                constexpr std::array<unsigned, 5> limits{2, 4, 8, 16, multiplayer_lobby_player_limit};
                const auto next = std::upper_bound(limits.begin(), limits.end(), s.capacity);
                s.capacity = next == limits.end() ? limits.front() : *next;
            }
        } else if (cmd == "voice-set") {
            if (s.voice_pending && GetTickCount64() >= s.voice_pending_until) s.voice_pending.reset();
            auto value = s.voice_pending.value_or(model.voice.settings);
            const auto& what = request.argument;
            if (what == "enabled") value.enabled = !value.enabled;
            else if (what == "open-mic") value.open_mic = !value.open_mic;
            else if (what == "proximity") value.proximity = !value.proximity;
            else if (what == "distance") value.distance = next_step(value.distance, voice_distances);
            else if (what == "volume") value.volume = next_step(value.volume, voice_volumes);
            else if (what == "microphone") value.microphone = next_step(value.microphone, voice_volumes);
            else continue;
            const auto argument = std::to_string(value.enabled) + " " + std::to_string(value.proximity) + " " +
                std::to_string(value.push_to_talk) + " " + std::to_string(value.distance) + " " + std::to_string(value.volume) + " " +
                std::to_string(value.open_mic) + " " + std::to_string(value.controller_combo) + " " + std::to_string(value.microphone);
            if (queue_command("voice", argument, {})) {
                s.voice_pending = value;
                s.voice_pending_until = GetTickCount64() + 2000;
                s.feedback.clear();
            } else s.feedback = "Couldn't apply that change. Try again.";
        } else if (cmd == "voice-player-volume") {
            const auto player = std::find_if(model.voice.players.begin(), model.voice.players.end(),
                [&](const auto& entry) { return std::to_string(entry.id) == request.argument; });
            const auto volume = next_step(player == model.voice.players.end() ? 1.f : player->volume, voice_volumes);
            s.feedback = queue_command("voice-volume", request.argument + " " + std::to_string(volume), {}) ? "" :
                "Couldn't apply that change. Try again.";
        } else if (cmd == "tps") {
            if (!model.active && !model.lobby_joining) {
                const auto next = std::upper_bound(multiplayer_tick_rates.begin(), multiplayer_tick_rates.end(), s.tps);
                s.tps = next == multiplayer_tick_rates.end() ? multiplayer_tick_rates.front() : *next;
            }
        } else {
            std::string command = cmd, argument = request.argument, password;
            if (cmd == "host" || cmd == "join" || cmd == "join-lobby") {
                // From a session they joined a player can hop to a server in the list (can_join).
                const bool hop = cmd == "join-lobby" && model.active && !model.hosting && !model.echo;
                if ((model.active && !hop) || model.lobby_joining) { s.feedback = "Leave the current session before connecting."; continue; }
                if (cmd == "host") {
                    if (!model.local_ready) { s.feedback = "Load a map before hosting."; continue; }
                    const auto name = input_text(context, "host-name");
                    if (name.size() > 128) { s.feedback = "Lobby name is too long. Use a shorter name."; continue; }
                    command = "host-config";
                    argument = std::string(s.public_lobby ? "public " : "code ") + std::to_string(s.capacity) +
                        " " + std::to_string(s.tps) + " " + name;
                    password = input_text(context, "host-password");
                } else {
                    if (cmd == "join") password = input_text(context, "join-password");
                    if (cmd == "join") {
                        argument = input_text(context, "join-code");
                        if (!s.protected_lobby.empty()) { command = "join-lobby"; argument = s.protected_lobby; }
                    }
                    if (command == "join-lobby") {
                        const auto lobby = std::find_if(model.lobbies.begin(), model.lobbies.end(), [&](const auto& entry) {
                            return std::to_string(entry.id) == argument;
                        });
                        if (lobby == model.lobbies.end() || !menu_view::can_join(model, *lobby)) {
                            SecureZeroMemory(password.data(), password.size());
                            s.feedback = "That lobby is unavailable. Refresh the browser."; continue;
                        }
                        if (cmd == "join-lobby" && lobby->password_required) {
                            SecureZeroMemory(password.data(), password.size());
                            s.protected_lobby = argument;
                            clear_input(context, "join-password");
                            s.section = Section::join;
                            context.set(context.path(s.page, {content, 0x18f8355b}), static_cast<int>(s.section));
                            continue;
                        }
                    }
                }
            }
            if (password.size() > 64) {
                SecureZeroMemory(password.data(), password.size());
                s.feedback = "Password is too long. Use a shorter password."; continue;
            }
            if ((cmd == "kick" || cmd == "object-placement" || cmd == "object-limit" || cmd == "clear-objects" || cmd == "world-layer-sync" ||
                 cmd == "voice-allow" || cmd == "noclip-allow" || cmd == "nobail-allow" || cmd == "boosts-allow" ||
                 cmd == "tuning-enforce") &&
                !model.hosting) {
                s.feedback = "Only the host can change this setting."; continue;
            }
            const bool accepted = queue_command(command, argument, password);
            if (!password.empty()) SecureZeroMemory(password.data(), password.size());
            s.feedback = accepted ? "" : "Couldn't apply that change. Try again.";
            s.feedback_status = model.status;
            s.feedback_until = GetTickCount64() + 4000;
            if (accepted && cmd == "host") clear_input(context, "host-password");
            if (accepted && (cmd == "join" || cmd == "join-lobby")) clear_input(context, "join-password");
        }
    }
}
void render_section(const Context& context, const MultiplayerModel& model, Section section) {
    auto& s = state();
    // Reuse the ReSkate page's catalog snapshot; no extra model reads or scans.
    const auto& levels = page_state(1).tools_model.levels;
    const bool idle = !model.active && !model.lobby_joining;
    const auto index = static_cast<unsigned>(section);
    std::vector<std::string> main, side;
    s.row_width = main_width;
    if (section == Section::browser) {
        auto query = input_text(context, "browser-search");
        if (query != s.browser.query) s.browser.query = std::move(query);
        const auto result = menu_view::browse(model, s.browser, levels);
        add_text(context, main, "browser-heading", "SERVERS");
        add_text(context, main, "browser-count", std::to_string(result.total) + " servers and public lobbies");
        if (result.lobbies.empty()) {
            add_text(context, main, "browser-empty", model.lobby_searching ? "Finding skaters..." :
                !model.lobby_searched ? "Find your next session." : model.lobbies.empty() ?
                "No public lobbies right now." : "No lobbies match your filters.");
            add_text(context, main, "browser-help", !model.lobby_searched ? "Choose Refresh lobbies to see who is skating." :
                model.lobbies.empty() ? "Host a public lobby and invite other skaters." : "Try a different name or map.");
        }
        for (const auto* lobby : result.lobbies) {
            const auto id = std::to_string(lobby->id);
            const bool available = menu_view::can_join(model, *lobby);
            const auto title = menu_view::caption(lobby->name) + (lobby->official ? "   [OFFICIAL]" : lobby->dedicated ? "   [SERVER]" : "") + "\n" + menu_view::caption(menu_view::map_name(lobby->map, levels), 32) +
                "   /   " + std::to_string(lobby->players) + "/" + std::to_string(lobby->capacity) +
                " skaters   /   " + (lobby->players >= lobby->capacity ? "FULL" : lobby->password_required ? "PASSWORD" : "OPEN") +
                (lobby->friends.empty() ? std::string{} : "   /   WITH " + menu_view::caption(lobby_friends_text(*lobby), 40));
            add_button(context, main, "lobby-" + id, title, available ? "join-lobby" : "", id);
        }
        s.row_width = side_width;
        add_text(context, side, "browser-tools", "FIND A SESSION");
        add_input(context, side, "browser-search", "Search lobby or map");
        add_button(context, side, "browser-map", s.browser.same_map ? "Map: Current map" : "Map: All maps", "filter-map");
        constexpr std::array<const char*, 3> sorts{"Lobby name", "Most players", "Map"};
        add_button(context, side, "browser-sort", std::string("Sort: ") + sorts[static_cast<unsigned>(s.browser.sort)], "sort");
        add_button(context, side, "refresh", model.lobby_searching ? "Searching..." : "Refresh lobbies",
            model.lobby_searching ? "" : "browse", "", true);
        add_text(context, side, "browser-status", s.feedback.empty() ? model.browser_status : s.feedback);
        if (!idle) add_text(context, side, "browser-connected", model.active && !model.hosting && !model.echo
            ? "Pick another server to leave this one and join it." : "End your session before joining another.");
    } else if (section == Section::host) {
        add_text(context, main, "host-heading", "MAKE IT YOUR SESSION");
        add_text(context, main, "host-name-label", "LOBBY NAME");
        add_input(context, main, "host-name", "Your Steam name", model.local_name);
        add_text(context, main, "host-password-label", "PASSWORD (OPTIONAL)");
        add_input(context, main, "host-password", "Leave blank for an open lobby");
        add_text(context, main, "host-map", "MAP  /  " + menu_view::map_name(model.map, levels));
        s.row_width = side_width;
        add_text(context, side, "host-options", "LOBBY SETTINGS");
        add_button(context, side, "visibility", (model.hosting ? model.public_host : s.public_lobby) ?
            "Visibility: Public" : "Visibility: Join code", idle ? "visibility" : "");
        add_button(context, side, "capacity", "Skaters: " + std::to_string(model.hosting ? model.capacity : s.capacity), idle ? "capacity" : "");
        add_button(context, side, "tps", "Tick rate: " + std::to_string(model.hosting ? model.tps : s.tps), idle ? "tps" : "");
        add_text(context, side, "visibility-help", s.public_lobby ? "Listed in the public server browser." : "Only skaters with your code can join.");
        add_button(context, side, "host", !idle ? "Session already active" : model.local_ready ? "Host lobby" : "Load a map to host",
            idle && model.local_ready ? "host" : "", "", true);
        add_text(context, side, "host-status", s.feedback.empty() ? model.status : s.feedback);
    } else if (section == Section::join) {
        const auto lobby = std::find_if(model.lobbies.begin(), model.lobbies.end(), [&](const auto& entry) {
            return std::to_string(entry.id) == s.protected_lobby;
        });
        const bool protected_join = !s.protected_lobby.empty();
        add_text(context, main, "join-heading", protected_join ? "PASSWORD REQUIRED" : "SKATE WITH YOUR FRIENDS");
        if (protected_join) {
            add_text(context, main, "join-target", lobby != model.lobbies.end() ? menu_view::caption(lobby->name) : "Lobby is no longer listed. Refresh the browser.");
        } else {
            add_text(context, main, "join-code-label", "JOIN CODE");
            add_input(context, main, "join-code", "Paste a ReSkate join code");
        }
        add_text(context, main, "join-password-label", "LOBBY PASSWORD");
        add_input(context, main, "join-password", protected_join ? "Enter the lobby password" : "Only needed for protected lobbies");
        s.row_width = side_width;
        add_text(context, side, "join-info", protected_join ? "JOIN THIS LOBBY" : "GOT A CODE?");
        add_text(context, side, "join-help", protected_join ? "Enter the password shared by the host." : "Ask the host for their session code.");
        add_text(context, side, "join-travel", "You will travel to the host's map.");
        add_button(context, side, "join", model.lobby_joining ? "Joining..." : "Join lobby",
            idle && (!protected_join || lobby != model.lobbies.end()) ? "join" : "", "", true);
        if (protected_join) add_button(context, side, "join-code-mode", "Use a join code instead", "code-mode");
        add_text(context, side, "join-status", s.feedback.empty() ? model.status : s.feedback);
    } else if (section == Section::voice) {
        if (s.voice_pending && (*s.voice_pending == model.voice.settings || GetTickCount64() >= s.voice_pending_until))
            s.voice_pending.reset();
        const auto value = s.voice_pending.value_or(model.voice.settings);
        add_text(context, main, "voice-heading", "SKATERS IN YOUR SESSION");
        bool anyone{};
        for (const auto& player : model.roster) {
            if (player.id == model.local_id || !player.connected) continue;
            anyone = true;
            const auto id = std::to_string(player.id);
            const auto found = std::find_if(model.voice.players.begin(), model.voice.players.end(),
                [&](const auto& entry) { return entry.id == player.id; });
            const auto voice = found == model.voice.players.end() ? VoicePlayer{player.id} : *found;
            add_button(context, main, "voice-mute-" + id, menu_view::caption(player.name, 32) + "  /  " +
                (voice.muted ? "MUTED" : model.voice.allowed && voice.speaking ? "SPEAKING" : "Mute"),
                "voice-mute", id + (voice.muted ? " off" : " on"), false, 112.f);
            add_button(context, main, "voice-volume-" + id, "Volume: " + volume_label(voice.volume),
                "voice-player-volume", id, false, 112.f);
        }
        if (!anyone) {
            add_text(context, main, "voice-alone", model.active ? "No other skaters in the session yet." : "You are not in a session yet.");
            add_text(context, main, "voice-alone-help", "These voice settings apply to every session you join.");
        }
        s.row_width = side_width;
        add_text(context, side, "voice-settings", "VOICE SETTINGS", 48.f);
        add_button(context, side, "voice-enabled", std::string("Voice chat: ") + (value.enabled ? "On" : "Off"),
            "voice-set", "enabled", false, 136.f);
        add_button(context, side, "voice-mode", std::string("Transmit: ") + (value.open_mic ? "Open mic" : "Push to talk"),
            "voice-set", "open-mic", false, 136.f);
        if (!value.open_mic) {
            std::string binds = "Talk key: " + voice_key_name(value.push_to_talk);
            if (value.controller_combo) binds += "  /  Pad: " + controller_combo_label(value.controller_combo);
            add_text(context, side, "voice-binds", binds, 48.f);
        }
        add_button(context, side, "voice-proximity", std::string("Proximity voice: ") + (value.proximity ? "On" : "Off"),
            "voice-set", "proximity", false, 136.f);
        add_button(context, side, "voice-distance", "Voice distance: " + std::to_string(static_cast<int>(value.distance + .5f)) + " m",
            value.proximity ? "voice-set" : "", "distance", false, 136.f);
        add_button(context, side, "voice-volume", "Listening volume: " + volume_label(value.volume), "voice-set", "volume", false, 136.f);
        add_button(context, side, "voice-microphone", "Microphone volume: " + volume_label(value.microphone),
            "voice-set", "microphone", false, 136.f);
        if (model.hosting)
            add_button(context, side, "voice-allow", std::string("Lobby voice: ") + (model.voice.allowed ? "Allowed" : "Disabled"),
                "voice-allow", model.voice.allowed ? "off" : "on", false, 136.f);
        else if (model.active && !model.voice.allowed)
            add_text(context, side, "voice-disallowed", "The host has disabled voice chat for this lobby.", 48.f);
        add_text(context, side, "voice-status", !s.feedback.empty() ? s.feedback :
            model.voice.transmitting ? "MICROPHONE TRANSMITTING" : model.voice.status, 48.f);
    } else {
        add_text(context, main, "session-heading", model.active ? menu_view::caption(model.lobby_name.empty() ? "CURRENT SESSION" : model.lobby_name) : "YOUR SESSION");
        if (!model.active) {
            add_text(context, main, "no-session", "You are not in a session yet.");
            add_text(context, main, "session-help", "Find a public lobby, host your own, or join with a code.");
            add_button(context, main, "session-browse", "Find a public lobby", "section", "browser", true);
        } else {
            add_text(context, main, "session-roster", "SKATERS  /  " + std::to_string(model.players) + "/" + std::to_string(model.capacity));
            add_button(context, main, "local-player", menu_view::caption(model.local_name) +
                (model.hosting ? "  /  YOU - HOST" : model.parties && model.party_leader ? "  /  YOU - PARTY LEADER" : "  /  YOU"),
                {}, {}, false, 112.f);
            for (const auto& player : model.roster) {
                if (player.id == model.local_id || !player.connected) continue;
                const auto id = std::to_string(player.id);
                // Your party members are marked.
                const std::string party = !model.parties || !player.party_member ? ""
                                        : player.party_leader ? "  /  PARTY LEADER" : "  /  PARTY";
                add_button(context, main, "player-" + id, menu_view::caption(player.name) +
                    (player.id == model.host_id ? "  /  HOST" : party), "select-player", menu_view::player_identity(player), false, 112.f);
            }
        }
        s.row_width = side_width;
        const auto* selected = menu_view::selected_player(model, s.selected_player);
        if (!selected) s.selected_player.clear();
        if (selected) {
            add_text(context, side, "player-options", "PLAYER OPTIONS");
            add_text(context, side, "selected-player-name", menu_view::caption(selected->name, 32));
            add_text(context, side, "selected-player-role", selected->id == model.host_id ? "Lobby host"
                : model.parties && selected->party_member ? (selected->party_leader ? "Your party's leader" : "In your party")
                : model.parties && selected->party ? "In another party" : "Connected skater");
            if (model.hosting && selected->id != model.host_id)
                add_button(context, side, "remove-player", "Remove from session", "kick", menu_view::player_identity(*selected));
            if (model.parties) {
                const auto id = std::to_string(selected->id);
                if (!selected->party_member) {
                    add_button(context, side, "party-invite", "Invite to party", "party", "invite " + id);
                    if (selected->party) add_button(context, side, "party-join", "Join their party", "party", "join " + id);
                } else if (model.party_leader) {
                    add_button(context, side, "party-kick", "Remove from party", "party", "kick " + id);
                    add_button(context, side, "party-promote", "Make party leader", "party", "promote " + id);
                }
            }
            add_button(context, side, "back-to-session", "Session details", "session-details");
        } else if (model.active) {
            add_text(context, side, "session-details", "SESSION DETAILS", 48.f);
            add_text(context, side, "map", menu_view::map_name(model.map, levels), 48.f);
            add_text(context, side, "invite-label", "JOIN CODE", 48.f);
            add_text(context, side, "invite", model.invite, 80.f);
            add_text(context, side, "session-tps", "Tick rate: " + std::to_string(model.tps), 48.f);
            if (model.parties) {
                // Parties: yours, and the invites waiting for an answer.
                for (const auto& invite : model.party_invites) {
                    const auto from = std::to_string(invite.from);
                    add_text(context, side, "party-invite-" + from, menu_view::caption(invite.name, 32) + " invited you to their party", 48.f);
                    add_button(context, side, "party-accept-" + from, "Accept party invite", "party", "accept " + from, false, 136.f);
                    add_button(context, side, "party-decline-" + from, "Decline", "party", "decline " + from, false, 136.f);
                }
                if (model.party) {
                    if (model.party_leader)
                        add_button(context, side, "party-privacy", std::string("Party: ") + (model.party_open ? "Anyone can join" : "Invite only"),
                            "party", model.party_open ? "close" : "open", false, 136.f);
                    add_button(context, side, "party-leave", "Leave party", "party", "leave", false, 136.f);
                } else {
                    add_text(context, side, "party-none", "Not in a party: select a skater to invite them.", 48.f);
                }
            }
            const auto placement = model.dedicated && model.object_placement == ObjectPlacement::host_only
                ? std::string("Admins only") : std::string(object_placement_name(model.object_placement));
            add_button(context, side, "editor", "Object placement: " + placement,
                model.hosting ? "object-placement" : "", "next", false, 136.f);
            {
                // The same round numbers as the overlay's list, one press each.
                static constexpr std::array<unsigned, 7> limits{0, 10, 25, 50, 100, 250, 500};
                const auto found = std::find(limits.begin(), limits.end(), model.object_limit);
                const auto next = limits[found == limits.end() ? 0 : static_cast<std::size_t>(found - limits.begin() + 1) % limits.size()];
                add_button(context, side, "object-limit",
                    "Objects per player: " + (model.object_limit ? std::to_string(model.object_limit) : std::string("No limit")),
                    model.hosting ? "object-limit" : "", next ? std::to_string(next) : std::string("off"), false, 136.f);
            }
            if (model.hosting)
                add_button(context, side, "clear-objects", "Delete all guest objects", "clear-objects", {}, false, 136.f);
            add_button(context, side, "guest-noclip", std::string("Guest noclip: ") + (model.guest_noclip ? "Allowed" : "Off"),
                model.hosting ? "noclip-allow" : "", "toggle", false, 136.f);
            add_button(context, side, "guest-nobail", std::string("Guest No Bail: ") + (model.guest_no_bail ? "Allowed" : "Off"),
                model.hosting ? "nobail-allow" : "", "toggle", false, 136.f);
            add_button(context, side, "guest-boosts", std::string("Guest boosts: ") + (model.guest_boosts ? "Allowed" : "Off"),
                model.hosting ? "boosts-allow" : "", "toggle", false, 136.f);
            add_button(context, side, "tuning", std::string("Physics tuning: ") +
                (model.enforce_tuning ? (model.dedicated ? "Game's" : "Host's") : "Everyone's own"),
                model.hosting ? "tuning-enforce" : "", "toggle", false, 136.f);
            add_button(context, side, "layers", std::string("World layer sync: ") + (model.force_world_layers ? "On" : "Off"),
                model.hosting ? "world-layer-sync" : "", "toggle", false, 136.f);
            add_button(context, side, "nametags", std::string("Nametags: ") + (model.nametags ? "On" : "Off"),
                "nametags", "toggle", false, 136.f);
            add_button(context, side, "chat-bubbles", std::string("Chat bubbles: ") + (model.chat_bubbles ? "On" : "Off"),
                "chat-bubbles", "toggle", false, 136.f);
            add_button(context, side, "chat-bubbles-own", std::string("Own chat bubbles: ") + (model.chat_bubbles_own ? "On" : "Off"),
                model.chat_bubbles ? "chat-bubbles-own" : "", "toggle", false, 136.f);
            add_button(context, side, "copy-code", "Copy join code", model.invite.empty() ? "" : "copy-code", {}, false, 136.f);
            add_button(context, side, "leave", model.hosting ? "End session" : "Leave session", "stop", {}, false, 136.f);
        }
        add_text(context, side, "session-status", s.feedback.empty() ? model.status : s.feedback, 48.f);
    }
    publish_rows(context, s.lists[index], index * 2, main);
    publish_rows(context, s.side_lists[index], index * 2 + 1, side);
}
} // namespace native_menu_detail
} // namespace dingosdk::multiplayer
