#include "session_internal.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include "Extension/Multiplayer/Hud/native_indicators.h"
#include "Extension/Multiplayer/Hud/native_player_ui.h"
#include "Extension/Multiplayer/Hud/custom_nametags.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Engine/Core/Platform/launcher_support.h"
#include "Engine/Core/Text/word_filter.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <ctime>

namespace dingosdk::multiplayer {
using namespace session_detail;
namespace session_detail {
void apply_distances(Session &s, const MultiplayerDistances &distances) {
    if (s.distances == distances) return;
    s.distances = distances;
    // Re-evaluate every recipient using the new boundaries, without carrying a
    // previous band's hysteresis or waiting for its old send deadline.
    for (auto &peer : active_peers(s)) peer.pose_delivery = {};
}
void apply_object_limit(Session &s, unsigned limit) {
    s.object_limit = limit;
    set_lobby_object_limit(s.mode == Mode::host || s.server_admin ? 0 : limit);
}
void apply_object_placement(Session &s, ObjectPlacement policy) {
    s.object_placement = policy;
    // On a dedicated server "host only" means its admins.
    set_lobby_object_placement_allowed(object_placement_allowed(policy, s.mode == Mode::host || s.server_admin));
}
void apply_nametags(const Session &s) {
    // The feed also runs when only chat bubbles are on: the overlay draws whichever of the
    // two is enabled from the flags it is handed.
    // Nametags are ReSkate's own (Hud/custom_nametags.h); the game's are never shown. Its
    // compass comes back when they are off, since they are what points at the other players.
    set_custom_nametags_enabled(s.nametags || s.chat_bubbles);
    set_native_nametags_enabled(false);
    set_native_compass_enabled(!s.nametags);
}
void apply_guest_tools(Session &s, bool noclip, bool no_bail, bool boosts) {
    s.guest_noclip = noclip;
    s.guest_no_bail = no_bail;
    s.guest_boosts = boosts;
    const bool exempt = s.mode == Mode::host || s.server_admin;
    set_session_tools_allowed(noclip || exempt, no_bail || exempt, boosts || exempt);
}
} // namespace session_detail
namespace {
std::string edit_distances(Session &s, std::string_view argument) {
    if (s.mode != Mode::host) return "Only the lobby host can change TPS distances.";
    MultiplayerDistances value;
    for (auto *field : {&value.full_rate_return, &value.half_rate_start, &value.half_rate_return, &value.low_rate_start}) {
        const auto first = argument.find_first_not_of(" \t");
        if (first == std::string_view::npos) return "Enter all four TPS distances in metres.";
        argument.remove_prefix(first);
        const auto end = argument.find_first_of(" \t");
        const auto token = argument.substr(0, end);
        const auto result = std::from_chars(token.data(), token.data() + token.size(), *field);
        if (result.ec != std::errc{} || result.ptr != token.data() + token.size())
            return "TPS distances must be whole metres between 0 and 10000.";
        argument.remove_prefix(token.size());
    }
    if (argument.find_first_not_of(" \t") != std::string_view::npos || !value.valid())
        return "Use ordered distances: return to full TPS < drop to 10 <= return to 10 < drop to 5 (maximum 10000 m).";
    apply_distances(s, value);
    s.roster_dirty = true; // Reliable host roster distributes these on the next ready network tick.
    load_host_preferences(s);
    s.host_preferences.distances = value;
    save_host_preferences(s);
    return "TPS distances applied to the lobby. Connected players and new joiners use the host's settings.";
}
// Native widgets retain their activation callback. Resolve toggle requests
// against live state, never a value captured when the widget opened.
std::optional<bool> parse_switch(std::string_view argument, bool current) {
    if (argument == "toggle") return !current;
    if (argument == "on" || argument == "off") return argument == "on";
    return {};
}
// ---- teleports ----
std::string lowered(std::string_view text) {
    std::string result(text);
    for (auto &c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}
std::string peer_name(Session &s, const Peer &p) {
    return p.member.name.empty() ? s.transport.name(p.member.id) : p.member.name;
}
// The one other player a name start (or SteamID64) means; null with `error` set otherwise.
Peer *find_player(Session &s, std::string_view who, std::string &error) {
    std::uint64_t id{};
    const auto parsed = std::from_chars(who.data(), who.data() + who.size(), id);
    const bool numeric = parsed.ec == std::errc{} && parsed.ptr == who.data() + who.size();
    Peer *match{};
    for (auto &p : active_peers(s)) {
        if (!p.handshaken || !p.member.id || (dedicated_host(s) && p.member.id == s.host_id)) continue;
        if (numeric ? p.member.id == id : lowered(peer_name(s, p)).starts_with(lowered(who))) {
            if (match) { error = "More than one player matches \"" + std::string(who) + "\"."; return nullptr; }
            match = &p;
        }
    }
    if (!match) error = "No player matches \"" + std::string(who) + "\".";
    return match;
}
// party: "invite|join|kick|promote <player>", "accept|decline [player]", "leave", "open",
// "close" or "status". Menus pass SteamID64s; people type the start of a name.
std::string party_command(Session &s, std::string_view argument) {
    while (!argument.empty() && argument.front() == ' ') argument.remove_prefix(1);
    const auto space = argument.find(' ');
    const auto verb = lowered(argument.substr(0, space));
    auto who = space == std::string_view::npos ? std::string_view{} : argument.substr(space + 1);
    while (!who.empty() && who.front() == ' ') who.remove_prefix(1);
    if (s.mode != Mode::host && s.mode != Mode::join) return "Parties need a multiplayer session.";
    const auto name = [&](std::uint64_t id) {
        auto *p = find_peer(s, id);
        return p ? peer_name(s, *p) : s.transport.name(id);
    };
    if (verb.empty() || verb == "status" || verb == "list") {
        if (!s.local_party) return "You're not in a party. Invite a player from their player card or with: party invite <name>.";
        std::string text = std::string("Your party") + (s.local_party_open ? " (open):" : ":");
        const auto local = s.transport.status().local_id;
        text += " " + s.transport.name(local) + (s.local_party_leader ? " (leader)" : "");
        for (const auto &peer : active_peers(s))
            if (peer.handshaken && peer.member.id && party_member(s, peer.member.id))
                text += ", " + peer_name(s, peer) + (peer.member.party_leader ? " (leader)" : "");
        return text;
    }
    static constexpr std::pair<std::string_view, PartyAction> verbs[] = {
        {"invite", PartyAction::invite}, {"accept", PartyAction::accept}, {"decline", PartyAction::decline},
        {"join", PartyAction::join},     {"leave", PartyAction::leave},   {"kick", PartyAction::kick},
        {"remove", PartyAction::kick},   {"promote", PartyAction::promote}, {"leader", PartyAction::promote},
        {"open", PartyAction::open},     {"close", PartyAction::close}};
    const auto found = std::find_if(std::begin(verbs), std::end(verbs), [&](const auto &v) { return v.first == verb; });
    if (found == std::end(verbs)) return "party invite|accept|decline|join|leave|kick|promote|open|close|status";
    const auto action = found->second;
    // The game re-applies its direct-join setting now and then: only a change is sent.
    if ((action == PartyAction::open && s.local_party_open) || (action == PartyAction::close && !s.local_party_open))
        return {};
    std::uint64_t player{};
    if (action == PartyAction::accept || action == PartyAction::decline) {
        // Without a name: the newest invite.
        if (who.empty()) {
            if (s.party_invites.empty()) return "You have no party invites.";
            player = s.party_invites.back().from;
        }
    }
    if (!player && action != PartyAction::leave && action != PartyAction::open && action != PartyAction::close) {
        if (who.empty()) return "Name a player.";
        std::string error;
        auto *peer = find_player(s, who, error);
        if (!peer) return error;
        player = peer->member.id;
    }
    auto result = send_party_request(s, action, player);
    if (!result.empty()) return result;
    if (action == PartyAction::accept || action == PartyAction::decline) {
        std::erase_if(s.party_invites, [&](const auto &invite) { return invite.from == player; });
        ++s.party_revision;
    }
    switch (action) {
    case PartyAction::invite: return "Inviting " + name(player) + "...";
    case PartyAction::accept: return "Joining " + name(player) + "'s party...";
    case PartyAction::decline: return "Declined " + name(player) + "'s party invite.";
    default: return {};
    }
}
// tp: the local skater to another player (beside them) or to x y z.
std::string teleport_self(Session &s, std::string_view argument) {
    if (!session_noclip_allowed()) return "The host has turned off noclip and teleporting in this session.";
    std::array<float, 3> at{};
    unsigned numbers{};
    for (auto rest = argument; numbers < 3;) {
        while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
        if (rest.empty()) break;
        const auto end = rest.find(' ');
        const auto word = rest.substr(0, end);
        const auto parsed = std::from_chars(word.data(), word.data() + word.size(), at[numbers]);
        if (parsed.ec != std::errc{} || parsed.ptr != word.data() + word.size()) break;
        ++numbers;
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end);
    }
    if (numbers == 3) {
        char text[96]{};
        std::snprintf(text, sizeof text, "Teleporting to (%.1f, %.1f, %.1f).", at[0], at[1], at[2]);
        return dingosdk::teleport_local_skater(at) ? text : "Teleporting is unavailable right now.";
    }
    if (argument.empty()) return "tp <player> or tp <x> <y> <z>";
    if (s.mode != Mode::host && s.mode != Mode::join) return "Teleporting to a player needs a multiplayer session.";
    std::string error;
    auto *p = find_player(s, argument, error);
    if (!p) return error;
    if (!p->visible) return peer_name(s, *p) + " is not in the world right now.";
    at = p->render_pose.root.position;
    at[0] += 2.0f; // beside them, not inside
    at[1] += 1.0f;
    return dingosdk::teleport_local_skater(at) ? "Teleporting to " + peer_name(s, *p) + "."
                                                      : "Teleporting is unavailable right now.";
}
// tpall / tphere: other players to the host, in a ring around them. A dedicated server's
// admins ask the server instead (see command()).
std::string teleport_players(Session &s, std::string_view action, std::string_view argument) {
    if (s.mode != Mode::host) return "Only the host or a server admin can teleport other players.";
    if (!s.local_root) return "Your skater is not in the world yet.";
    std::vector<Peer *> players;
    if (action == "tphere") {
        if (argument.empty()) return "tphere <player>";
        std::string error;
        auto *p = find_player(s, argument, error);
        if (!p) return error;
        players.push_back(p);
    } else {
        for (auto &p : active_peers(s))
            if (p.handshaken && p.member.id && p.world_ready) players.push_back(&p);
        if (players.empty()) return "Nobody else is in the world.";
    }
    const auto &at = s.local_root->position;
    const auto now = now_us();
    unsigned sent{};
    for (std::size_t i = 0; i < players.size(); ++i) {
        const float angle = 6.2831853f * static_cast<float>(i) / static_cast<float>(players.size());
        auto p = packet(s, PacketKind::teleport, now);
        p.teleport = {at[0] + 2.5f * std::cos(angle), at[1] + 1.0f, at[2] + 2.5f * std::sin(angle)};
        if (send_packet(s, players[i]->member.id, p, true, false)) ++sent;
    }
    return players.size() == 1 && sent ? "Teleported " + peer_name(s, *players[0]) + " to you."
                                       : "Teleported " + std::to_string(sent) + " player(s) to you.";
}
enum class GuestTool { noclip, no_bail, boosts };
std::string edit_guest_tool(Session &s, std::string_view argument, GuestTool tool) {
    const char *name = tool == GuestTool::noclip ? "noclip and teleporting" : tool == GuestTool::no_bail ? "No Bail" : "boosts";
    if (s.mode != Mode::host) return std::string("Only the session host can change who may use ") + name + ".";
    auto &current = tool == GuestTool::noclip ? s.guest_noclip : tool == GuestTool::no_bail ? s.guest_no_bail : s.guest_boosts;
    const auto allowed = parse_switch(argument, current);
    if (!allowed) return "Choose on, off or toggle.";
    auto noclip = s.guest_noclip, no_bail = s.guest_no_bail, boosts = s.guest_boosts;
    (tool == GuestTool::noclip ? noclip : tool == GuestTool::no_bail ? no_bail : boosts) = *allowed;
    apply_guest_tools(s, noclip, no_bail, boosts);
    auto &remembered = s.host_preferences;
    remembered.guest_noclip = noclip;
    remembered.guest_no_bail = no_bail;
    remembered.guest_boosts = boosts;
    save_host_preferences(s);
    s.roster_dirty = true;
    return std::string(*allowed ? "Guests may use " : "Guests can no longer use ") + name + ".";
}
std::string edit_guest_noclip(Session &s, std::string_view argument) { return edit_guest_tool(s, argument, GuestTool::noclip); }
std::string edit_guest_no_bail(Session &s, std::string_view argument) { return edit_guest_tool(s, argument, GuestTool::no_bail); }
std::string edit_guest_boosts(Session &s, std::string_view argument) { return edit_guest_tool(s, argument, GuestTool::boosts); }
std::string edit_enforce_tuning(Session &s, std::string_view argument) {
    if (s.mode != Mode::host) return "Only the session host can choose whose physics tuning guests skate with.";
    const auto enforce = parse_switch(argument, s.enforce_tuning);
    if (!enforce) return "Choose on, off or toggle.";
    s.enforce_tuning = *enforce;
    s.host_preferences.enforce_tuning = *enforce;
    save_host_preferences(s);
    s.roster_dirty = true;
    s.next_tuning_check = 0;
    return *enforce ? "Guests skate with your physics tuning." : "Guests skate with their own physics tuning.";
}
std::string edit_score_check(Session &s, std::string_view argument) {
    if (dedicated_host(s)) return "The server decides: its admins use \"server score-check off|warn|kick\".";
    if (s.mode != Mode::host) return "Only the session host can choose whether mods that change scoring or physics are checked.";
    const auto check = parse_switch(argument, s.score_check);
    if (!check) return "Choose on, off or toggle.";
    s.score_check = *check;
    s.host_preferences.score_check = *check;
    save_host_preferences(s);
    for (auto &peer : active_peers(s))
        if (peer.handshaken) judge_scoring(s, peer);
    s.roster_dirty = true;
    s.next_scoring_check = 0; // the host's own flag follows on the next tick
    return *check ? "Players whose mods change scoring or physics are kept out of throwdowns and coop challenges."
                  : "Mods that change scoring or physics are no longer checked.";
}
std::string edit_object_placement(Session &s, std::string_view argument) {
    if (s.mode != Mode::host) return "Only the session host can change object placement.";
    const auto policy = parse_object_placement(argument, s.object_placement);
    if (!policy) return "Use everyone, host, nobody, or next for object placement.";
    // Clients lock their editor and native tools when the roster arrives, but
    // enforcement is the host freezing guest layouts (publish_guest_objects).
    apply_object_placement(s, *policy);
    s.roster_dirty = true;
    load_host_preferences(s);
    s.host_preferences.placement = *policy;
    save_host_preferences(s);
    return *policy == ObjectPlacement::everyone ? "Everyone can place and edit objects."
         : *policy == ObjectPlacement::host_only ? "Only you can place objects. Guests' objects are frozen."
                                                 : "Object placement is disabled for everyone. Existing objects stay.";
}
std::string edit_object_limit(Session &s, std::string_view argument) {
    if (s.mode != Mode::host) return "Only the session host can change the object limit.";
    const auto limit = parse_object_limit(argument);
    if (!limit) return "Use a number of objects from 1 to " + std::to_string(max_object_limit) + ", or off.";
    // Guests' games stop them at the limit when the roster arrives; enforcement is the host
    // showing everyone no more than that of each guest's layout (publish_guest_objects).
    apply_object_limit(s, *limit);
    for (auto &peer : active_peers(s)) peer.shared_from = 0; // look at every layout again
    s.roster_dirty = true;
    load_host_preferences(s);
    s.host_preferences.object_limit = *limit;
    save_host_preferences(s);
    return *limit ? "Each guest can place up to " + std::to_string(*limit) + " objects."
                  : std::string("Guests can place as many objects as they like.");
}
// Removes every guest's objects for everyone, whatever the placement policy.
// The host's own objects are its saved park and are left alone.
std::string clear_guest_objects(Session &s, std::string_view) {
    if (s.mode != Mode::host) return "Only the session host can delete guest objects.";
    std::size_t removed{};
    for (auto &peer : active_peers(s)) {
        if (!peer.handshaken) continue;
        for (const auto *state : {&peer.objects, &peer.shared})
            for (const auto &[id, object] : state->objects()) {
                (void)object;
                peer.cleared.insert(id);
            }
        removed += peer.shared.objects().size();
        if (peer.shared.revision()) peer.shared.replace({});
        peer.shared_from = peer.objects.revision();
    }
    s.object_clears = s.object_clears.value_or(0) + 1;
    s.roster_dirty = true;
    return removed ? "Deleted " + std::to_string(removed) + " guest object" + (removed == 1 ? "" : "s") + "."
                   : "Guests have no placed objects to delete.";
}
std::string edit_nametags(Session &s, std::string_view argument) {
    const auto enabled = parse_switch(argument, s.nametags);
    if (!enabled) return "Use on, off, or toggle for peer nametags.";
    s.nametags = *enabled;
    s.display_preferences_loaded = true;
    apply_nametags(s);
    profile_runtime::set_local_preference("Nametags", s.nametags);
    return s.nametags ? "Peer nametags shown." : "Peer nametags hidden.";
}
// The Special page: a player on one of the backend's lists going without their tag, or
// without the animation on their items. Their next appearance packet tells everyone
// (session_send.cpp).
std::string edit_own_tag(Session &, std::string_view argument) {
    const auto shown = parse_switch(argument, own_tag_shown());
    if (!shown) return "Use on, off, or toggle for your tag.";
    show_own_tag(*shown);
    profile_runtime::set_local_preference("IdentityTag", *shown);
    return *shown ? "Your tag shows." : "Your tag is hidden, for you and everyone you skate with.";
}
std::string edit_own_items(Session &, std::string_view argument) {
    const auto shown = parse_switch(argument, own_items_shown());
    if (!shown) return "Use on, off, or toggle for your items.";
    show_own_items(*shown);
    profile_runtime::set_local_preference("IdentityItems", *shown);
    return *shown ? "Your items animate." : "Your items are plain, for you and everyone you skate with.";
}
// The Special page: how one of the player's marked cosmetics is coloured, as
// "<cosmetic> <mode> <rrggbb> <rrggbb> <speed>" (MarkStyle). It goes out with their next
// appearance packet and is kept in their profile.
std::string edit_mark_style(Session &, std::string_view argument) {
    std::istringstream in{std::string(argument)};
    unsigned item{}, mode{}, from{}, to{}, speed{};
    if (!(in >> item >> mode >> std::hex >> from >> to >> std::dec >> speed) || item >= mark_items || mode > 4 ||
        from > 0xffffff || to > 0xffffff || speed > 2)
        return "That is not a cosmetic style.";
    // Mode 4, the rainbow, is the staff's to pick (a developer's standard is it already). It is
    // kept, and sent, as a solid colour older builds can show (rainbow_style).
    const auto social = steam_social_snapshot();
    const auto mark = social ? identity_mark(social->local.id) : std::nullopt;
    if (mode == 4 && mark != IdentityList::staff) return "The rainbow is not one of your styles.";
    const auto colour = [](unsigned value) {
        return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(value >> 16), static_cast<std::uint8_t>(value >> 8),
                                           static_cast<std::uint8_t>(value)};
    };
    auto styles = developer_hoodie_detail::own_styles.load();
    styles[item] = {static_cast<MarkMode>(mode == 4 ? 3 : mode), colour(from), colour(to), static_cast<std::uint8_t>(speed)};
    if (mode == 4) styles[item].from = developer_hoodie_detail::standard_picks(*mark).first, styles[item].to = rainbow_marker;
    // A solid colour whose unused second colour happens to be the rainbow's marker is not one.
    else if (rainbow_style(styles[item])) styles[item].to[2] ^= 1;
    developer_hoodie_detail::own_styles.store(styles);
    profile_runtime::set_local_values({{"IdentityStyles", Json(developer_hoodie_detail::mark_styles_text(styles))}});
    return std::string(mark_item_names[item]) +
           (mode == 1 ? ": off." : mode == 2 ? ": your gradient." : mode == 3 ? ": your color." : mode == 4 ? ": rainbow."
                                                                                                            : ": back to the usual.");
}
// Hidden chat still receives lines, so showing it again brings back the conversation.
std::string edit_chat_visible(Session &s, std::string_view argument) {
    const auto visible = parse_switch(argument, s.chat_visible);
    if (!visible) return "Use on, off, or toggle for text chat.";
    s.chat_visible = *visible;
    profile_runtime::set_local_preference("ChatVisible", s.chat_visible);
    publish_chat(s);
    return s.chat_visible ? "Text chat shown." : "Text chat hidden.";
}
std::string edit_chat_filter(Session &s, std::string_view argument) {
    const auto filter = parse_switch(argument, s.chat_filter);
    if (!filter) return "Use on, off, or toggle for the chat filter.";
    s.chat_filter = *filter;
    profile_runtime::set_local_preference("ChatFilter", s.chat_filter);
    publish_chat(s);
    return s.chat_filter ? "Bad words in chat are hidden." : "Chat is shown unfiltered.";
}
std::string edit_player_distance(Session &s, std::string_view argument) {
    float value{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !std::isfinite(value) ||
        value < player_distance_least || value > player_distance_unlimited)
        return "Player distance is a number of metres from 50 to 1000 (1000: every player).";
    s.player_distance = value;
    s.next_shown_rank = 0;
    profile_runtime::set_local_values({{"PlayerDistance", static_cast<double>(value)}});
    return value >= player_distance_unlimited ? std::string("Every player is shown as a skater, however far.")
                                              : "Players within " + std::to_string(static_cast<int>(value)) + " m are shown as skaters.";
}
std::string start_pose_dump(Session &s, std::string_view argument) {
    int seconds{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || seconds < 1 || seconds > 600)
        return "pose-dump <seconds> (1-600): records your own poses while in a session.";
    std::error_code error;
    const auto folder = std::filesystem::current_path(error) / "logs";
    std::filesystem::create_directories(folder, error);
    const auto file = folder / ("poses-" + std::to_string(now_us() / 1000000) + ".bin");
    s.pose_dump.close();
    s.pose_dump.clear();
    s.pose_dump.open(file, std::ios::binary | std::ios::trunc);
    if (!s.pose_dump) return "Could not write " + file.string() + ".";
    s.pose_dump.write("RSPD1\n", 6);
    s.pose_dump_until = now_us() + static_cast<std::uint64_t>(seconds) * 1000000;
    s.pose_dump_count = 0;
    return "Recording your poses for " + std::to_string(seconds) + " s to " + file.string() + ". Skate as you normally would.";
}
std::string edit_direct_connections(Session &s, std::string_view argument) {
    const auto enabled = parse_switch(argument, s.prefer_direct);
    if (!enabled) return "Use on, off, or toggle for direct connections.";
    s.prefer_direct = *enabled;
    profile_runtime::set_local_preference("DirectConnections", s.prefer_direct);
    return s.prefer_direct ? "Servers that offer it are connected to directly (from the next join)."
                                : "Every server is reached through Steam's relays (from the next join).";
}
std::string edit_nametag_distance(Session &s, std::string_view argument) {
    float value{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !std::isfinite(value) ||
        value < 10.f || value > 500.f)
        return "Nametag distance is a number of metres from 10 to 500.";
    s.nametag_distance = value;
    profile_runtime::set_local_values({{"NametagDistance", static_cast<double>(value)}});
    return "Names show within " + std::to_string(static_cast<int>(value)) + " m.";
}
std::string edit_nametag_dots(Session &s, std::string_view argument) {
    const auto enabled = parse_switch(argument, s.nametag_dots);
    if (!enabled) return "Use on, off, or toggle for nametag dots.";
    s.nametag_dots = *enabled;
    profile_runtime::set_local_preference("NametagDots", s.nametag_dots);
    return s.nametag_dots ? "Far and off-screen players show as dots." : "No dots for far and off-screen players.";
}
std::string edit_nametags_friends(Session &s, std::string_view argument) {
    const auto enabled = parse_switch(argument, s.nametags_friends);
    if (!enabled) return "Use on, off, or toggle for friends-only nametags.";
    s.nametags_friends = *enabled;
    profile_runtime::set_local_preference("NametagsFriendsOnly", s.nametags_friends);
    return s.nametags_friends ? "Only your Steam friends have nametags." : "Every player has a nametag.";
}
std::string edit_chat_bubbles(Session &s, std::string_view argument) {
    const auto enabled = parse_switch(argument, s.chat_bubbles);
    if (!enabled) return "Use on, off, or toggle for chat bubbles.";
    s.chat_bubbles = *enabled;
    s.display_preferences_loaded = true;
    apply_nametags(s);
    profile_runtime::set_local_preference("ChatBubbles", s.chat_bubbles);
    return s.chat_bubbles ? "Chat bubbles show above skaters." : "Chat bubbles hidden.";
}
std::string edit_chat_bubbles_own(Session &s, std::string_view argument) {
    const auto enabled = parse_switch(argument, s.chat_bubbles_own);
    if (!enabled) return "Use on, off, or toggle for your own chat bubbles.";
    s.chat_bubbles_own = *enabled;
    profile_runtime::set_local_preference("ChatBubblesOwn", s.chat_bubbles_own);
    return s.chat_bubbles_own ? "Your own messages show as bubbles too."
                              : "Only other players' messages show as bubbles.";
}
std::string edit_chat_bubbles_distance(Session &s, std::string_view argument) {
    float value{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !std::isfinite(value) ||
        value < 5.f || value > 500.f)
        return "Chat bubble distance is a number of metres from 5 to 500.";
    s.chat_bubbles_distance = value;
    profile_runtime::set_local_values({{"ChatBubblesDistance", static_cast<double>(value)}});
    return "Chat bubbles show within " + std::to_string(static_cast<int>(value)) + " m.";
}
std::string edit_chat_bubbles_duration(Session &s, std::string_view argument) {
    float value{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !std::isfinite(value) ||
        value < 1.f || value > 30.f)
        return "Chat bubble duration is a number of seconds from 1 to 30.";
    s.chat_bubbles_duration = value;
    profile_runtime::set_local_values({{"ChatBubblesDuration", static_cast<double>(value)}});
    return "Chat bubbles last " + std::to_string(static_cast<int>(value)) + " seconds.";
}
std::string edit_chat_bubbles_history(Session &s, std::string_view argument) {
    int value{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || value < 1 || value > 8)
        return "Chat bubble history is a number of lines from 1 to 8.";
    s.chat_bubbles_history = value;
    profile_runtime::set_local_values({{"ChatBubblesHistory", static_cast<std::int64_t>(value)}});
    return "Chat bubbles stack up to " + std::to_string(value) + " recent line(s).";
}
std::string kick_player(Session &s, std::string_view argument) {
    if (s.mode != Mode::host) return "Only the session host can kick players.";
    const auto split = argument.find(' ');
    if (split == std::string_view::npos) return "Select a connected player to kick.";
    const auto identity = argument.substr(0, split), generation = argument.substr(split + 1);
    std::uint64_t id{}, epoch{};
    const auto parsed_id = std::from_chars(identity.data(), identity.data() + identity.size(), id);
    const auto parsed_epoch = std::from_chars(generation.data(), generation.data() + generation.size(), epoch);
    if (!id || !epoch || parsed_id.ec != std::errc{} || parsed_epoch.ec != std::errc{} ||
        parsed_id.ptr != identity.data() + identity.size() || parsed_epoch.ptr != generation.data() + generation.size())
        return "Select a connected player to kick.";
    if (id == s.host_id) return "The host cannot kick themselves. Use Disconnect to end the session.";
    const auto *peer = find_peer(s, id);
    if (!peer || !peer->handshaken || peer->member.epoch != epoch)
        return "That player left or rejoined. Select them from the current player list.";
    const auto name = peer->member.name.empty() ? std::to_string(id) : peer->member.name;
    s.banned.insert(id);
    disconnect(s, id, "You were kicked by the session host.");
    s.next_party_update = 0;
    return name + " was kicked and cannot rejoin this session.";
}
// "ban <SteamID64> [name]": works with or without a session, so the list can be
// managed any time; a banned player who is connected now is dropped at once.
std::string ban_player(Session &s, std::string_view argument) {
    const auto split = argument.find(' ');
    const auto identity = argument.substr(0, split);
    std::uint64_t id{};
    const auto parsed = std::from_chars(identity.data(), identity.data() + identity.size(), id);
    // Individual SteamID64s start at 76561197960265728 (universe 1, individual account).
    if (parsed.ec != std::errc{} || parsed.ptr != identity.data() + identity.size() || id < 76561197960265728ULL ||
        id > 76561202255233023ULL)
        return "Enter a SteamID64: the 17-digit number starting 7656119.";
    const auto local = s.transport.status().local_id;
    if (id == local || (s.mode == Mode::host && id == s.host_id)) return "You cannot ban yourself.";
    auto name = split == std::string_view::npos ? std::string{} : clean_chat_text(argument.substr(split + 1));
    if (name.size() > 64) {
        std::size_t cut = 64;
        while (cut && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
        name.resize(cut);
    }
    const auto *peer = find_peer(s, id);
    if (name.empty() && peer) name = peer->member.name;
    load_bans(s);
    if (is_banned(s, id)) return (name.empty() ? std::to_string(id) : name) + " is already banned.";
    s.bans.push_back({id, name, static_cast<std::int64_t>(std::time(nullptr))});
    s.ban_ids_dirty = true;
    save_bans(s);
    const auto label = name.empty() ? std::to_string(id) : name;
    if (s.mode == Mode::host && peer && peer->handshaken) {
        disconnect(s, id, "You were banned by the session host.");
        s.next_party_update = 0;
        return label + " was banned and removed from the session.";
    }
    return label + " is banned from lobbies you host.";
}
std::string unban_player(Session &s, std::string_view argument) {
    std::uint64_t id{};
    const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), id);
    if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size()) return "Choose a banned player.";
    load_bans(s);
    const auto found = std::find_if(s.bans.begin(), s.bans.end(), [&](const auto &ban) { return ban.id == id; });
    if (found == s.bans.end()) return "That player is not banned.";
    const auto label = found->name.empty() ? std::to_string(id) : found->name;
    s.bans.erase(found);
    s.ban_ids_dirty = true;
    save_bans(s);
    // An unbanned player can also come back to the session they were kicked from.
    s.banned.erase(id);
    return label + " was unbanned.";
}
std::string edit_world_layer_sync(Session &s, std::string_view argument) {
    if (s.mode != Mode::host) return "Only the session host can change world layer sync.";
    const auto enabled = parse_switch(argument, s.force_world_layers);
    if (!enabled) return "Use on, off, or toggle for world layer sync.";
    s.force_world_layers = *enabled;
    s.layers = local_profile_world_layers().choices;
    s.roster_dirty = true;
    load_host_preferences(s);
    s.host_preferences.world_layer_sync = *enabled;
    save_host_preferences(s);
    return s.force_world_layers ? "Everyone now follows the host's world layer choices."
                                : "World layer sync disabled. Guests regain their own layer choices.";
}
// Sends one setting change to the dedicated server this guest is an admin of.
// The server answers in chat.
std::string send_admin(Session &s, std::string text) {
    auto *host = find_peer(s, s.host_id);
    if (!host || !host->handshaken) return "Not connected to the server yet.";
    if (!s.server_admin) return "You are not an admin on this server.";
    auto request = packet(s, PacketKind::admin, now_us());
    request.text = std::move(text);
    if (request.text.size() > max_admin_text || !valid_admin_text(request.text)) return "That request is too long.";
    if (!send_packet(s, s.host_id, request, true, false)) return "Could not reach the server.";
    return "Sent to the server.";
}
// The Special page's commands. They are a player's own offline as well, where their hoodie and
// board still animate.
bool own_mark_command(std::string_view action) {
    return action == "mark-tag" || action == "mark-items" || action == "mark-style";
}
} // namespace
bool queue_command(std::string_view action, std::string_view argument, std::string_view password) {
    if (launcher::offline_mode() && !own_mark_command(action)) return false;
    if ((action != "host" && action != "host-config" && action != "join" && action != "join-lobby" && action != "join-friend-lobby" && action != "stop" &&
         action != "distances" && action != "object-placement" && action != "object-limit" && action != "kick" && action != "clear-objects" &&
         action != "nametags" && action != "chat-visible" && action != "chat-filter" &&
         action != "nametag-distance" && action != "nametag-dots" && action != "nametags-friends" && action != "player-distance" && action != "direct-connections" && action != "pose-dump" &&
         action != "chat-bubbles" && action != "chat-bubbles-own" && action != "chat-bubbles-distance" &&
         action != "chat-bubbles-duration" && action != "chat-bubbles-history" &&
         !own_mark_command(action) &&
         action != "voice" && action != "voice-mute" &&
         action != "voice-volume" && action != "voice-allow" && action != "voice-range" && action != "chat" && action != "ban" && action != "unban" &&
         action != "world-layer-sync" && action != "noclip-allow" && action != "nobail-allow" && action != "boosts-allow" &&
         action != "tuning-enforce" && action != "tp" &&
         action != "tpall" && action != "tphere" && action != "browse" &&
         action != "server" && action != "party") ||
        argument.size() > (action == "host" || action == "host-config" ? 160U : action == "chat" ? 4 * multiplayer_chat_max_bytes
                           : action == "server" ? max_admin_text : 128U) ||
        password.size() > 64)
        return false;
    auto &s = session();
    auto request = std::make_unique<PrivateRequest>();
    request->action = action;
    request->argument = argument;
    request->password = password;
    request->queued = now_us();
    std::lock_guard lock(s.request_mutex);
    if (s.requests.size() >= 4)
        return false;
    s.requests.push_back(std::move(request));
    return true;
}
std::string command(std::string_view action, std::string_view argument, std::string_view password) {
    if (launcher::offline_mode() && !own_mark_command(action))
        return "Multiplayer is unavailable in offline mode. Start Steam and relaunch ReSkate.";
    const bool configured_host = action == "host-config";
    if (configured_host) action = "host";
    PrivateRequest input;
    input.password = password;
    auto &s = session();
    try {
        // A dedicated server's admin changes the server's settings instead of
        // their own: the same menu actions, sent to the server. "server" sends
        // any server console command.
        if (dedicated_host(s) && (action == "server" || (s.server_admin &&
            (action == "distances" || action == "object-placement" || action == "object-limit" || action == "voice-allow" || action == "voice-range" ||
             action == "clear-objects" || action == "kick" || action == "ban" || action == "unban" ||
             action == "world-layer-sync" || action == "noclip-allow" || action == "nobail-allow" ||
             action == "tpall" || action == "tphere" || action == "boosts-allow" || action == "tuning-enforce")))) {
            const auto text = action == "server" ? std::string(argument) : std::string(action) + " " + std::string(argument);
            const auto result = send_admin(s, text);
            if (result != "Sent to the server.") add_chat(s, 0, "Server", result);
            return result;
        }
        if (action == "server") return "Server commands need a dedicated server session.";
        if (action == "chat" && !argument.empty() && argument.front() == '/') {
            // A command: /help and /tp run here; the rest (votes, admin commands) go to a
            // dedicated server, which answers in chat.
            const auto line = argument.substr(1);
            const auto space = line.find(' ');
            const auto verb = lowered(line.substr(0, space));
            const auto rest = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
            std::string result;
            if (verb.empty() || verb == "help" || verb == "?") {
                for (const auto &c : chat_commands(s)) add_chat(s, 0, "ReSkate", c.usage + "  " + c.description);
            } else if (verb == "tp") {
                result = teleport_self(s, rest);
                if (!result.empty()) add_chat(s, 0, "ReSkate", result);
                return result;
            } else if (!dedicated_host(s) && verb == "p") {
                // A lobby's host relays party chat, as a dedicated server does.
                const auto refused = send_party_chat(s, rest);
                if (!refused.empty() && (s.mode == Mode::host || s.mode == Mode::join)) add_chat(s, 0, "ReSkate", refused);
                return refused.empty() ? "Message sent." : refused;
            } else if (!dedicated_host(s) && verb == "party") {
                result = party_command(s, rest);
            } else {
                result = send_chat_command(s, argument);
            }
            if (!result.empty() && (s.mode == Mode::host || s.mode == Mode::join)) add_chat(s, 0, "ReSkate", result);
            return result.empty() ? "Command sent." : result;
        }
        if (action == "chat") {
            // The overlay queues chat and never sees this result, so a refusal
            // is shown in the chat itself.
            const auto refused = send_chat(s, argument);
            if (!refused.empty() && (s.mode == Mode::host || s.mode == Mode::join)) add_chat(s, 0, "ReSkate", refused);
            return refused.empty() ? "Message sent." : refused;
        }
        if (action == "party") {
            const auto result = party_command(s, argument);
            if (!result.empty() && (s.mode == Mode::host || s.mode == Mode::join)) add_chat(s, 0, "ReSkate", result);
            publish(s);
            return result.empty() ? "Sent to the server." : result;
        }
        if (action == "voice") {
            VoiceSettings value;
            int enabled{}, proximity{}, key{}, open_mic{};
            std::uint32_t controller{};
            float distance{}, volume{}, microphone{};
            const auto number = [&](auto &target) {
                const auto first = argument.find_first_not_of(" \t");
                if (first == std::string_view::npos) return false;
                argument.remove_prefix(first);
                const auto token = argument.substr(0, argument.find_first_of(" \t"));
                const auto result = std::from_chars(token.data(), token.data() + token.size(), target);
                if (result.ec != std::errc{} || result.ptr != token.data() + token.size()) return false;
                argument.remove_prefix(token.size());
                return true;
            };
            if (!number(enabled) || !number(proximity) || !number(key) || !number(distance) || !number(volume) ||
                !number(open_mic) || !number(controller) || !number(microphone) ||
                argument.find_first_not_of(" \t") != std::string_view::npos ||
                (enabled != 0 && enabled != 1) || (proximity != 0 && proximity != 1) ||
                (open_mic != 0 && open_mic != 1)) return "Invalid voice settings.";
            value.enabled = enabled != 0; value.proximity = proximity != 0;
            value.push_to_talk = key; value.distance = distance; value.volume = volume;
            value.open_mic = open_mic != 0; value.controller_combo = controller; value.microphone = microphone;
            if (!value.valid()) return "Invalid voice key, distance or volume.";
            s.voice_settings = value;
            s.voice.configure(value);
            publish(s);
            return value.enabled ? (value.open_mic ? "Open microphone enabled." : "Push-to-talk voice enabled.") : "Voice chat disabled.";
        }
        if (action == "tp") return teleport_self(s, argument);
        if (action == "tpall" || action == "tphere") return teleport_players(s, action, argument);
        if (action == "voice-allow") {
            if (s.mode != Mode::host) return "Only the host can change lobby voice permissions.";
            if (argument != "on" && argument != "off") return "Choose on or off.";
            const bool allowed = argument == "on";
            if (s.voice_policy.allowed != allowed) {
                s.voice_policy.allowed = allowed;
                if (!++s.voice_policy.revision) ++s.voice_policy.revision;
                s.voice.reset();
                s.roster_dirty = true;
            }
            publish(s);
            return allowed ? "Voice chat allowed for the lobby." : "Voice chat disabled for everyone.";
        }
        if (action == "voice-range") {
            float range{};
            const auto parsed = std::from_chars(argument.data(), argument.data() + argument.size(), range);
            if (parsed.ec != std::errc{} || parsed.ptr != argument.data() + argument.size() || !valid_voice_range(range))
                return "Choose a voice range from 50 to 1000 m.";
            load_host_preferences(s);
            s.host_preferences.voice_range = range;
            save_host_preferences(s);
            if (s.mode == Mode::host) s.voice_range = range;
            publish(s);
            return "Voice range set to " + std::to_string(static_cast<int>(range)) + " m.";
        }
        if (action == "voice-volume") {
            const auto split = argument.find(' ');
            if (split == std::string_view::npos) return "Choose a player and volume.";
            std::uint64_t id{};
            float volume{};
            const auto player = std::from_chars(argument.data(), argument.data() + split, id);
            const auto gain = std::from_chars(argument.data() + split + 1, argument.data() + argument.size(), volume);
            if (player.ec != std::errc{} || player.ptr != argument.data() + split || !find_peer(s, id) ||
                gain.ec != std::errc{} || gain.ptr != argument.data() + argument.size() || !valid_voice_volume(volume))
                return "Choose a connected player and a volume from 0 to 10.";
            s.voice.volume(id, volume);
            publish(s);
            return "Player voice volume updated.";
        }
        if (action == "voice-mute") {
            const auto split = argument.find(' ');
            if (split == std::string_view::npos) return "Choose a player to mute.";
            std::uint64_t id{};
            const auto result = std::from_chars(argument.data(), argument.data() + split, id);
            const auto enabled = argument.substr(split + 1);
            if (result.ec != std::errc{} || result.ptr != argument.data() + split || !find_peer(s, id) ||
                (enabled != "on" && enabled != "off")) return "Choose a connected player to mute.";
            s.voice.mute(id, enabled == "on");
            publish(s);
            return enabled == "on" ? "Player voice muted." : "Player voice unmuted.";
        }
        // Host-only settings check the mode themselves; guests get a refusal.
        using Setting = std::string (*)(Session &, std::string_view);
        static constexpr std::pair<std::string_view, Setting> settings[] = {
            {"object-placement", edit_object_placement}, {"object-limit", edit_object_limit}, {"kick", kick_player},
            {"noclip-allow", edit_guest_noclip},           {"nobail-allow", edit_guest_no_bail},
            {"boosts-allow", edit_guest_boosts},           {"tuning-enforce", edit_enforce_tuning},
            {"score-check", edit_score_check},
            {"ban", ban_player},                           {"unban", unban_player},
            {"clear-objects", clear_guest_objects},
            {"world-layer-sync", edit_world_layer_sync},   {"distances", edit_distances},
            {"nametags", edit_nametags},
            {"nametag-distance", edit_nametag_distance}, {"nametag-dots", edit_nametag_dots},
            {"nametags-friends", edit_nametags_friends}, {"player-distance", edit_player_distance}, {"direct-connections", edit_direct_connections}, {"pose-dump", start_pose_dump},
            {"mark-tag", edit_own_tag}, {"mark-items", edit_own_items}, {"mark-style", edit_mark_style},
            {"chat-visible", edit_chat_visible}, {"chat-filter", edit_chat_filter},
            {"chat-bubbles", edit_chat_bubbles}, {"chat-bubbles-own", edit_chat_bubbles_own},
            {"chat-bubbles-distance", edit_chat_bubbles_distance},
            {"chat-bubbles-duration", edit_chat_bubbles_duration},
            {"chat-bubbles-history", edit_chat_bubbles_history}};
        for (const auto &[name, edit] : settings)
            if (action == name) {
                s.status = edit(s, argument);
                publish(s);
                return s.status;
            }
        if (action == "status")
            return s.status + " | Players " + std::to_string(player_count(s)) + "/" +
                   std::to_string(s.capacity);
        if (action == "stop") {
            stop(s, "Multiplayer stopped.");
            publish(s);
            return s.status;
        }
        if (action == "retry") {
            each_peer([&] {
                auto &p = s.peers[peer_slot];
                update_player_ui(s.base, nullptr, {});
                remove_remote(s.base);
                p.render_failed = false;
                p.far_interval = 0;
                p.last_cosmetic_apply = 0;
            });
            s.status = "Retrying remote skater creation.";
            publish(s);
            return s.status;
        }
        if (action == "browse") {
            if (!s.transport.open())
                s.status = s.transport.status().detail;
            else {
                s.lobbies.refresh(now_us());
                s.servers.refresh(now_us());
            }
            publish(s);
            return s.lobbies.status().browser;
        }
        if (action == "join-lobby" || action == "join-friend-lobby") {
            // A guest can hop straight to a server or lobby they pick in the browser: they leave
            // this one first. A host ends everyone's session by leaving, so they end it themselves;
            // and a Steam friend's join offer, which can arrive unasked, never replaces a session.
            if (s.mode != Mode::off && (s.mode != Mode::join || action != "join-lobby")) {
                s.status = s.mode == Mode::join ? "Leave your current session before joining another lobby."
                                                : "End your session before joining another lobby.";
                publish(s);
                return s.status;
            }
            if (s.lobbies.status().joining)
                return "A lobby join is already in progress.";
            std::uint64_t id{};
            const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), id);
            if (result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || !id)
                return "Invalid lobby selection. Refresh the browser.";
            if (s.mode == Mode::join) {
                if (id == s.joined_public_lobby) return "You are already there.";
                stop(s, "Switching servers...");
            }
            // A dedicated server's row carries its join code. It is a public listing like a
            // lobby's: friends are told the player is there, and can follow them in.
            if (const auto *server = s.servers.find(id)) {
                const auto code = server->code;
                auto reply = command("join", code, input.password);
                if (s.mode == Mode::join) {
                    s.joined_public_lobby = id;
                    publish(s);
                }
                return reply;
            }
            // A friend on a server this game has not listed yet: only the list has its code.
            if (game_server_steam_id(id)) {
                s.servers.refresh(now_us());
                s.status = "That server is not in your server list yet. Open the server browser and join it there.";
                publish(s);
                return s.status;
            }
            if (!s.transport.open())
                return s.transport.status().detail;
            if (action == "join-friend-lobby")
                s.lobbies.join_friend(id, s.transport.status().local_id, now_us());
            else
                s.lobbies.join(id, s.transport.status().local_id, now_us());
            erase_password(s.lobby_password);
            if (s.lobbies.status().joining)
                s.lobby_password = input.password;
            publish(s);
            return s.lobbies.status().browser;
        }
        if (action == "test") {
            s.transport.socket_test();
            s.status = s.transport.status().detail;
            publish(s);
            return s.status;
        }
        if (action != "host" && action != "join" && action != "echo")
            return "Unknown multiplayer action.";
        unsigned capacity = multiplayer_lobby_player_limit;
        unsigned tps = multiplayer_default_tps;
        std::string_view lobby_name;
        auto visibility = argument;
        // Menus queue host and join and never see this result: show a refusal as the status.
        const auto refuse = [&s](std::string reason) {
            s.status = std::move(reason);
            publish(s);
            return s.status;
        };
        // Joining is for the host or the server to refuse (a server may opt out of the bans).
        if (action == "host" && reskate_banned(s.transport.status().local_id)) return refuse(std::string(banned_notice));
        if (action == "host") {
            const auto space = argument.find(' ');
            if (space != std::string_view::npos) {
                visibility = argument.substr(0, space);
                const auto options = argument.substr(space + 1);
                const auto name_start = options.find(' ');
                const auto number = options.substr(0, name_start);
                if (name_start != std::string_view::npos) lobby_name = options.substr(name_start + 1);
                const auto result = std::from_chars(number.data(), number.data() + number.size(), capacity);
                if (result.ec != std::errc{} || result.ptr != number.data() + number.size() || capacity < 2 ||
                    capacity > multiplayer_lobby_player_limit)
                    return refuse("Choose a player limit from 2 to " + std::to_string(multiplayer_lobby_player_limit) + ".");
            }
            if (!visibility.empty() && visibility != "code" && visibility != "public")
                return refuse("Use mp host code <limit> [lobby name] or mp host public <limit> [lobby name].");
            if (configured_host) {
                const auto split = lobby_name.find(' ');
                const auto rate = lobby_name.substr(0, split);
                const auto result = std::from_chars(rate.data(), rate.data() + rate.size(), tps);
                if (result.ec != std::errc{} || result.ptr != rate.data() + rate.size() || !valid_multiplayer_tps(tps))
                    return refuse("Choose 20, 30, 60, or 120 TPS before hosting.");
                lobby_name = split == std::string_view::npos ? std::string_view{} : lobby_name.substr(split + 1);
            }
            const auto first = lobby_name.find_first_not_of(' ');
            lobby_name = first == std::string_view::npos ? std::string_view{}
                : lobby_name.substr(first, lobby_name.find_last_not_of(' ') - first + 1);
            if (lobby_name.size() > 128 || std::any_of(lobby_name.begin(), lobby_name.end(),
                [](unsigned char c) { return c < 32 || c == 127; }))
                return refuse("Lobby names must be at most 128 bytes with no control characters.");
            // A lobby browsers refuse to list is no use to anyone: say so while it can be fixed.
            if (text::contains_bad_words(lobby_name))
                return refuse("That lobby name contains blocked words. Choose another one.");
        }
        std::optional<Invite> invitation;
        if (action == "join") {
            invitation = parse_invite(argument);
            if (!invitation)
                return refuse("Invalid join code. Paste the complete SteamID-session code from the host.");
            if (blocked_server(invitation->steam_id)) return refuse(std::string(blocked_server_notice));
        }
        stop(s, "Starting multiplayer...");
        s.chat.clear(); // A new session starts with an empty chat.
        s.tps = tps;
        s.capacity = capacity;
        s.epoch = nonce();
        s.secret = invitation ? invitation->secret : nonce();
        if (action != "echo")
            s.password = password_key(input.password, s.secret);
        if (action == "echo") {
            s.mode = Mode::echo;
            s.peers[0].member = {1, s.epoch, "Local Echo"};
            s.peers[0].handshaken = true;
            note_slot(s, 0);
            s.status = "Local Echo: a delayed copy follows your recorded path at " + std::to_string(s.tps) + " TPS.";
        } else if (action == "host") {
            if (!s.transport.host(capacity)) {
                s.status = s.transport.status().detail;
                publish(s);
                return s.status;
            }
            s.mode = Mode::host;
            s.host_id = s.transport.status().local_id;
            // Falling back to the Steam name: that one is not the player's to retype, so mask it.
            s.lobby_name = lobby_name.empty() ? text::mask_bad_words(s.transport.name(s.host_id))
                                              : std::string(lobby_name);
            if (s.lobby_name.empty()) s.lobby_name = "ReSkate session";
            s.invite = format_invite({s.host_id, s.secret});
            s.public_host = visibility == "public";
            if (s.public_host)
                s.lobbies.host(s.invite, capacity, s.password.has_value(), s.lobby_name);
            // Remember this setup, and bring back the host options chosen last time.
            load_host_preferences(s);
            auto &remembered = s.host_preferences;
            remembered.public_lobby = s.public_host;
            remembered.capacity = capacity;
            remembered.tps = tps;
            remembered.lobby_name = std::string(lobby_name);
            remembered.password_required = s.password.has_value();
            apply_distances(s, remembered.distances);
            s.voice_range = remembered.voice_range;
            apply_object_placement(s, remembered.placement);
            apply_object_limit(s, remembered.object_limit);
            apply_guest_tools(s, remembered.guest_noclip, remembered.guest_no_bail, remembered.guest_boosts);
            s.enforce_tuning = remembered.enforce_tuning;
            s.score_check = remembered.score_check;
            if (remembered.world_layer_sync) {
                s.force_world_layers = true;
                s.layers = local_profile_world_layers().choices;
            }
            s.roster_dirty = true;
            save_host_preferences(s);
            s.status = "Hosting for up to " + std::to_string(capacity) + " players at " + std::to_string(s.tps) + " TPS.";
        } else {
            // A server that listens on its own address is connected to directly, with Steam's
            // relays as the fallback (steam_transport.h). The server sees the address of a
            // player who connects this way, as any dedicated server does, so the player may
            // turn it off (direct-connections). Preferences load with the first session frame,
            // so read this one here.
            const bool direct_allowed = s.display_preferences_loaded
                                            ? s.prefer_direct
                                            : profile_runtime::local_preference("DirectConnections").value_or(true);
            std::uint32_t direct_ip{};
            std::uint16_t direct_port{};
            if (const auto *listed = s.servers.find(invitation->steam_id);
                listed && direct_allowed && listed->direct_ip && listed->direct_port > 0) {
                direct_ip = listed->direct_ip;
                direct_port = static_cast<std::uint16_t>(listed->direct_port);
            }
            if (!s.transport.join(invitation->steam_id, direct_ip, direct_port)) {
                s.status = s.transport.status().detail;
                publish(s);
                return s.status;
            }
            s.mode = Mode::join;
            // Wait for the authenticated host's initial policy.
            apply_object_placement(s, ObjectPlacement::nobody);
            s.world = 0;
            s.host_id = invitation->steam_id;
            s.awaiting_map = true;
            s.join_started = now_us();
            s.status = "Connecting and checking the host's map...";
        }
        set_lobby_park_mode(s.mode == Mode::host || s.mode == Mode::join, s.mode == Mode::join);
        set_lobby_object_guest(s.mode == Mode::join);
        publish(s);
        return s.status;
    } catch (const std::exception &e) {
        stop(s, e.what());
        publish(s);
        return s.status;
    }
}
} // namespace dingosdk::multiplayer
