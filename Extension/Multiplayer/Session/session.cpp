#include "session_internal.h"
#include "Extension/Throwdowns/throwdown_kit.h"
#include "Extension/Skater/skater_push.h"
#include "Extension/Throwdowns/throwdown_lab.h"
#include "Engine/Core/Hooks/hooks.h"
#include "Extension/Plugins/plugin_runtime.h"
#include "Extension/Plugins/plugin_wire.h"
#include "Extension/Plugins/plugin_session_feed.h"
#include "Extension/AnimationStudio/anim_core.h"
#include "Engine/Core/Debug/trace.h"
#include "Extension/Multiplayer/Hud/native_player_ui.h"
#include "Extension/Multiplayer/Hud/native_indicators.h"
#include "Extension/Multiplayer/Hud/native_party.h"
#include "party_book.h"
#include "Engine/Game/World/world_names.h"
#include "Extension/Multiplayer/Remote/native_skater.h"
#include "Extension/Multiplayer/Hud/custom_nametags.h"
#include "Extension/Multiplayer/Hud/follow_camera.h"
#include "Extension/Throwdowns/native_throwdowns.h"
#include "Extension/Throwdowns/throwdown_relay.h"
#include "Extension/Progression/script_natives.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Extension/Objects/ParkEditor/park_editor_runtime.h"
#include "Extension/Multiplayer/Remote/native_audio.h"
#include "Extension/Multiplayer/Remote/native_vfx.h"
#include "Extension/Skater/client_source_spawn.h"
#include "Extension/Skater/no_bail.h"
#include "Extension/Skater/physics_tuning.h"
#include "Extension/Multiplayer/Remote/remote_collision.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Text/word_filter.h"
#include "Engine/Game/Multiplayer/session_tools.h"
#include "Engine/Game/UI/game_view.h"
#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace dingosdk::multiplayer {
using namespace session_detail;
namespace session_detail {
std::uint64_t nonce() {
    std::uint64_t value{};
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value), sizeof(value),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !value)
        throw std::runtime_error("Cannot generate a multiplayer session code.");
    return value;
}
Session &session() {
    static auto *s = new Session;
    return *s;
}
Peer *find_peer(Session &s, std::uint64_t id) {
    if (id)
        for (auto &p : active_peers(s))
            if (p.member.id == id)
                return &p;
    return nullptr;
}
unsigned player_count(const Session &s) {
    unsigned count = 1;
    for (const auto &p : active_peers(s))
        if (p.handshaken && !(dedicated_host(s) && p.member.id == s.host_id))
            ++count;
    return count;
}
void reset_peer(Session &s, std::size_t slot) {
    remove_remote_network_objects(s.peers[slot].member.id, s.peers[slot].member.epoch);
    const PeerScope scope(slot);
    update_player_ui(s.base, nullptr, {});
    remove_remote(s.base);
    s.peers[slot] = {};
}
void stop(Session &s, std::string reason) {
    s.local_typing_until = 0;
    s.next_presence = s.presence_for = 0;
    // A join that ends before the host's map was ready never got in.
    if (s.mode == Mode::join && s.awaiting_map && !s.travelling && s.join_started)
        trace::record("multiplayer.join", "app.join", std::chrono::milliseconds((now_us() - s.join_started) / 1000), false);
    s.voice.reset();
    s.voice_policy = {};
    s.tps = multiplayer_default_tps;
    s.object_placement = ObjectPlacement::everyone;
    s.object_limit = 0;
    s.object_scaling = true;
    s.sync_effects = true;
    set_lobby_object_limit(0);
    s.server_admin = false;
    s.role.clear();
    s.own_tag.clear();
    s.own_tag_color = 0;
    s.role_all_commands = false;
    s.role_commands.clear();
    s.role_permissions = 0;
    s.server_bans.clear();
    s.server_ban_total = 0;
    s.server_maps.clear();
    text::set_own_words({}, {}); // the server's own filtered words go with it
    s.chat_masked.clear();
    s.server_map_pool.clear();
    s.server_map_rotation = 0;
    set_lobby_object_placement_allowed(true);
    set_lobby_object_scaling_allowed(true);
    s.guest_noclip = s.guest_no_bail = s.guest_boosts = s.guest_ollies = true;
    s.enforce_tuning = true;
    s.score_check = true;
    s.scoring_sent.reset();
    s.local_scoring = false;
    s.next_scoring_check = 0;
    s.host_tuning.reset();
    s.sent_tuning.reset();
    s.tuning_packet.clear();
    s.next_tuning_check = 0;
    s.host_extras.reset();
    s.sent_extras = 0;
    s.extras_packet.clear();
    s.next_extras_check = 0;
    set_session_tuning_enforced(false);
    set_host_physics_extras({});
    s.server_votes = 0;
    s.server_polls = 0;
    s.server_custom_votes.clear();
    set_session_tools_allowed(true, true, true, true);
    s.object_clears.reset();
    s.clear_pending = false;
    s.force_world_layers = false;
    s.banned.clear();
    s.join_backoff = {};
    apply_host_world_layers(false, s.layers);
    s.layers = default_world_layers();
    clear_remote_network_objects();
    set_lobby_object_guest(false);
    s.local_objects = {}; s.next_object_update = 0;
    set_lobby_park_mode(false, false);
    s.parks = {}; s.next_park_update = 0;
    s.next_party_update = 0;
    s.parties = PartyBook{};
    s.parties_revision = 0;
    s.client_timing = {};
    s.next_publish = 0;
    s.last_client_log = 0;
    erase_key(s.password);
    erase_password(s.lobby_password);
    s.lobbies.stop();
    s.joined_public_lobby = 0;
    s.transport.stop();
    // Slots at or above used_slots were never used, or were reset when they were freed.
    for (std::size_t slot = 0; slot < s.used_slots; ++slot)
        reset_peer(s, slot);
    s.used_slots = 0;
    s.object_owners_valid = false;
    reset_audio();
    reset_effects();
    s.public_host = s.started_map = s.roster_dirty = false;
    s.awaiting_map = s.join_map_authorized = s.map_load_submitted = false;
    s.join_started = s.last_map_request = s.last_map_load_check = 0;
    s.join_destination.clear();
    s.map_label.clear();
    s.map_package.clear();
    s.required_plugins.clear();
    s.plugins_ready = s.fetching_with_map = false;
    s.fetching_since = 0;
    s.map_fetch_deadline = 0;
    map_fetch_deadline_flag.store(0);
    fetching_map_flag.store(false, std::memory_order_relaxed);
    s.world = 1;
    s.travelling = false;
    s.local_reload = s.local_reload_started = s.reload_ready_due = false;
    s.host_world_ready = true;
    s.travel_started = s.last_world_state = s.last_world_ready = 0;
    s.world_state_sequence = 0;
    s.direct_upload = {};
    s.distances = {};
    s.mode = Mode::off;
    s.invite.clear();
    s.lobby_name.clear();
    s.map_name.clear();
    s.native_status.clear();
    s.cosmetic_capture_status.clear();
    s.sent_appearance.reset();
    s.local_root.reset();
    s.last_skater_bones = s.last_board_bones = 0;
    s.cosmetic_packet.clear();
    s.map = s.context = s.parent = s.host_id = 0;
    s.next_send = s.last_hello = s.last_roster = s.last_cosmetic_capture = s.last_routes =
        s.last_network_log = 0;
    s.sequence = s.roster_sequence = 0;
    s.server_chat_badge = default_server_chat_badge;
    s.server_chat_text = default_server_chat_text;
    s.vote = {};
    s.vote_ends = 0;
    s.vote_mine = 0;
    s.announcement = {};
    s.announcement_ends = 0;
    server_vote_open_flag.store(false, std::memory_order_relaxed);
    server_poll_answers_flag.store(0, std::memory_order_relaxed);
    set_next_maps({});
    s.pose_streams.clear();
    s.pose_ack = {};
    s.pose_ack_due = false;
    s.pose_upload = {};
    s.own_poses.clear();
    s.sound_streams.clear();
    s.sound_upload = {};
    s.local_pose_count = s.logged_local_pose_count = 0;
    s.status = std::move(reason);
}
// Release world-owned objects and buffered state, keeping Steam connections,
// admission, password challenges and stable member slots intact.
void clear_world(Session &s, std::uint64_t now) {
    s.voice.reset();
    // The server starts its pose streams and message numbers over with the world.
    s.pose_streams.clear();
    s.pose_ack = {};
    s.pose_ack_due = false;
    s.pose_upload.restart();
    s.own_poses.clear();
    s.sound_streams.clear();
    s.sound_upload.restart();
    clear_remote_network_objects();
    s.object_owners_valid = false;
    s.local_objects = {}; s.next_object_update = 0;
    for (std::size_t slot = 0; slot < s.used_slots; ++slot) {
        const auto member = s.peers[slot].member;
        const bool admitted = s.peers[slot].handshaken;
        const bool authorized = s.peers[slot].map_authorized;
        const auto challenge = s.peers[slot].password_challenge;
        reset_peer(s, slot);
        auto &peer = s.peers[slot];
        peer.member = member;
        peer.handshaken = admitted;
        peer.map_authorized = authorized;
        peer.password_challenge = challenge;
        peer.world_ready = false;
        peer.travel_since = admitted ? now : 0;
        peer.fetching_since = 0;
        peer.last_packet = now;
    }
    reset_audio();
    reset_effects();
    s.started_map = false;
    s.context = s.parent = 0;
    s.sent_appearance.reset();
    s.local_root.reset();
    s.last_skater_bones = s.last_board_bones = 0;
    s.cosmetic_packet.clear();
    s.cosmetic_capture_status.clear();
    s.native_status.clear();
    s.next_send = s.last_cosmetic_capture = s.last_routes = s.last_world_ready = 0;
    s.roster_dirty = true;
}
bool world_playing(const Session &s, const NativeFrame &local) {
    return local.ready && !s.travelling && !s.awaiting_map && s.host_world_ready;
}
} // namespace session_detail
namespace {
void publish_party(Session &s) {
    const auto now = now_us();
    if (now < s.next_party_update) return;
    s.next_party_update = now + 100000;
    PartyRoster roster;
    if (s.mode != Mode::off) {
        expire_party_invites(s, now);
        const bool echo = s.mode == Mode::echo;
        // Who is talking, for the party voice HUD (UIPlayerInfo PlayerMic).
        const auto voice = s.voice.model();
        const auto mic = [&](std::uint64_t id) -> std::int8_t {
            if (!voice.ready || !voice.allowed) return -1;
            const auto found = std::find_if(voice.players.begin(), voice.players.end(), [&](const auto &p) { return p.id == id; });
            if (found == voice.players.end()) return -1;
            return found->muted ? 0 : found->speaking ? 2 : 1;
        };
        const auto local_id = echo ? 2ULL : s.transport.status().local_id;
        if (local_id) {
            const auto name = echo ? steam_social_snapshot()->local.name : s.transport.name(local_id);
            roster[0] = {local_id, s.epoch, name.empty() ? "Skater" : name,
                         true, echo || s.local_party_leader, s.gameplay_ready};
            roster[0].member = echo || s.local_party;
            roster[0].party = echo ? lobby_party : s.local_party;
            roster[0].leader_of_party = echo || s.local_party_leader;
            roster[0].party_open = !echo && s.local_party_open;
            roster[0].mic = voice.ready && voice.allowed && voice.settings.enabled ? (voice.transmitting ? 2 : 1) : -1;
            if (s.sent_appearance) roster[0].card = s.sent_appearance->card;
        }
        std::size_t index = 1;
        for (std::size_t slot = 0; slot < s.used_slots; ++slot) {
            const auto &peer = s.peers[slot];
            if (!peer.handshaken || !peer.member.id || (dedicated_host(s) && peer.member.id == s.host_id)) continue;
            const bool member = echo || party_member(s, peer.member.id);
            roster[index++] = {peer.member.id, peer.member.epoch, peer.member.name, false,
                               member && peer.member.party_leader, peer.visible && peer.world_ready, slot};
            roster[index - 1].member = member;
            roster[index - 1].party = echo ? lobby_party : peer.member.party;
            roster[index - 1].leader_of_party = !echo && peer.member.party_leader;
            // A party's openness is on its leader's roster entry.
            roster[index - 1].party_open = !echo && peer.member.party &&
                (peer.member.party == s.local_party ? s.local_party_open : [&] {
                    for (std::size_t other = 0; other < s.used_slots; ++other)
                        if (s.peers[other].handshaken && s.peers[other].member.party == peer.member.party &&
                            s.peers[other].member.party_leader)
                            return s.peers[other].member.party_open;
                    return false;
                }());
            roster[index - 1].mic = mic(peer.member.id);
            if (peer.appearance.value()) roster[index - 1].card = peer.appearance.value()->card;
        }
        // Native party colors follow member slots. Stable ordering keeps them
        // consistent on every client even though remote render slots differ:
        // party members first (leader first), then everyone else.
        // Entries from `index` on are empty and already last.
        std::sort(roster.begin(), roster.begin() + static_cast<std::ptrdiff_t>(index), [](const auto &a, const auto &b) {
            if (!a.id || !b.id) return a.id != 0 && b.id == 0;
            if (a.member != b.member) return a.member;
            if (a.leader != b.leader) return a.leader;
            return a.id < b.id;
        });
    }
    if (!s.display_preferences_loaded) {
        s.display_preferences_loaded = true;
        s.nametags = profile_runtime::local_preference("Nametags").value_or(true);
        if (const auto saved = profile_runtime::local_value("NametagDistance"); saved && saved->is_number())
            s.nametag_distance = std::clamp(saved->get<float>(), 10.f, 500.f);
        s.nametag_dots = profile_runtime::local_preference("NametagDots").value_or(true);
        s.prefer_direct = profile_runtime::local_preference("DirectConnections").value_or(true);
        if (const auto saved = profile_runtime::local_value("PlayerDistance"); saved && saved->is_number())
            s.player_distance = std::clamp(saved->get<float>(), player_distance_least, player_distance_unlimited);
        s.nametags_friends = profile_runtime::local_preference("NametagsFriendsOnly").value_or(false);
        s.chat_visible = profile_runtime::local_preference("ChatVisible").value_or(true);
        s.chat_filter = profile_runtime::local_preference("ChatFilter").value_or(true);
        s.chat_bubbles = profile_runtime::local_preference("ChatBubbles").value_or(true);
        s.chat_bubbles_own = profile_runtime::local_preference("ChatBubblesOwn").value_or(false);
        if (const auto saved = profile_runtime::local_value("ChatBubblesDistance"); saved && saved->is_number())
            s.chat_bubbles_distance = std::clamp(saved->get<float>(), 5.f, 500.f);
        if (const auto saved = profile_runtime::local_value("ChatBubblesDuration"); saved && saved->is_number())
            s.chat_bubbles_duration = std::clamp(saved->get<float>(), 1.f, 30.f);
        if (const auto saved = profile_runtime::local_value("ChatBubblesHistory"); saved && saved->is_number())
            s.chat_bubbles_history = std::clamp(static_cast<int>(saved->get<double>()), 1, 8);
        show_own_tag(profile_runtime::local_preference("IdentityTag").value_or(true));
        show_own_items(profile_runtime::local_preference("IdentityItems").value_or(true));
        if (const auto saved = profile_runtime::local_value("IdentityStyles"); saved && saved->is_string())
            if (const auto styles = developer_hoodie_detail::parse_mark_styles(saved->string()))
                developer_hoodie_detail::own_styles.store(*styles);
        apply_nametags(s);
    }
    // The party's limit: parties players form hold up to eight (the game's Party panel rows),
    // in a lobby as on a dedicated server.
    const bool session = s.mode == Mode::host || s.mode == Mode::join;
    const auto capacity = s.mode == Mode::off ? static_cast<unsigned>(max_players)
                        : session ? static_cast<unsigned>(PartyBook::default_limit) : s.capacity;
    // A party the player formed is the game's party (its Social menu, Coop button, beacons...).
    // Nobody is in one just for being in the same lobby or on the same server.
    set_native_party_changes(session);
    const bool shown = session ? s.local_party != 0 : s.mode != Mode::off;
    // What a dedicated server placed (its parks) says "Server", as its chat lines do.
    set_native_party_server(dedicated_host(s) ? s.host_id : 0, "Server");
    update_native_party(s.base, roster, capacity, shown);
}
// Links the players' own throwdowns (Extension/Throwdowns/throwdown_relay.cpp). Runs
// every tick, with no session too, so linked drops are removed when it ends. The input
// persists: its player list is rebuilt when the admitted players change, and names
// (Steam lookups) are refreshed once a second.
void relay_throwdowns(Session &s, bool in_world, const NativeFrame* /*offline_skater*/ = nullptr) {
    auto &input = s.throwdown_input;
    const bool in_session = s.mode == Mode::host || s.mode == Mode::join;
    const auto local = in_session ? s.transport.status().local_id : 0;
    const auto now = now_us();
    const bool names = local != input.local || now < s.throwdown_names_at || now - s.throwdown_names_at >= 1000000;
    if (names) s.throwdown_names_at = now;
    input.local = local;
    // A player the dedicated server flagged for game speed, or whose mods change trick scoring,
    // plays no linked throwdowns or challenges: they are left out here as if gone (and when it is
    // the local player, everyone is).
    const bool barred = s.local_speeding || s.local_scoring;
    const auto listed = [&](const Peer &peer) {
        return peer.handshaken && peer.member.id && !(dedicated_host(s) && peer.member.id == s.host_id) &&
               !peer.member.speeding && !peer.member.scoring && !barred;
    };
    input.barred = in_session && barred;
    bool changed = names;
    std::size_t count{};
    if (in_session)
        for (std::size_t slot = 0; slot < s.used_slots; ++slot)
            if (listed(s.peers[slot])) {
                changed |= count >= input.peers.size() || input.peers[count].id != s.peers[slot].member.id ||
                           input.peers[count].slot != slot;
                ++count;
            }
    if (changed || count != input.peers.size()) {
        input.peers.clear();
        if (in_session)
            for (std::size_t slot = 0; slot < s.used_slots; ++slot) {
                const auto &peer = s.peers[slot];
                if (listed(peer))
                    input.peers.push_back({peer.member.id, slot,
                                           peer.member.name.empty() ? s.transport.name(peer.member.id) : peer.member.name});
            }
    }
    input.world = in_session ? s.map * 0x100000001b3ULL ^ s.world : 0;
    input.in_world = in_world;
    // Coop challenges invite players near the local skater (and place the celebration).
    const auto me = in_session && in_world ? s.local_root : std::nullopt;
    input.position = me ? std::optional(me->position) : std::nullopt;
    for (auto &peer : input.peers) {
        const auto &other = s.peers[peer.slot].latest_root;
        float d2 = 1e12f;
        if (me && other)
            d2 = (other->position[0] - me->position[0]) * (other->position[0] - me->position[0]) +
                 (other->position[1] - me->position[1]) * (other->position[1] - me->position[1]) +
                 (other->position[2] - me->position[2]) * (other->position[2] - me->position[2]);
        peer.position = other ? std::optional(other->position) : std::nullopt;
        peer.nearby = d2 < 60.f * 60.f;
        peer.party = party_member(s, peer.id);
    }
    if (names) {
        input.local_name = in_session ? s.transport.name(local) : std::string(steam_social_snapshot()->local.name);
        if(!in_session) {
            const auto card=local_profile_player_card();
            if(!card.custom_name.empty())input.local_name=card.custom_name;
        }
    }
    // Plugin messages ride in throwdown packets too, told apart by their first byte
    // (Extension/Plugins/plugin_wire.h); they go to the plugins, never to the relay.
    for (const auto &[sender, message] : s.throwdown_inbox) {
        if (plugins::wire::is_plugin(message)) {
            // The authority's words (a plugin's server copy) come from the lobby's host or the
            // server only; a host hears none from its guests.
            if (plugins::wire::from_authority(message) && (s.mode == Mode::host || sender != s.host_id)) continue;
            plugins::receive_message(sender, message);
        }
        else receive_throwdown_relay(sender, message);
    }
    s.throwdown_inbox.clear();
    for (auto &message : tick_throwdown_relay(s.base, input)) send_throwdown(s, std::move(message));
    for (auto &message : plugins::take_outgoing()) send_throwdown(s, std::move(message));
    for (auto &text : take_throwdown_relay_notices()) add_chat(s, 0, "ReSkate", std::move(text));
}
// A player in the local coop celebration stands at their spot there: the whole pose moves,
// the board's rig anchor with it (offset_pose), or their board stays in the local player's hands.
Pose moved_by(Pose pose, const std::array<float, 3> &offset) {
    offset_pose(pose, offset);
    return pose;
}
// A skater the throwdown hides stays spawned (no respawn hitch every turn) but far below
// the world: its root, its placement joint (1), the board entity and the board rig all move down.
Pose out_of_sight(Pose pose) {
    offset_pose(pose, {0.0f, -2000.0f, 0.0f});
    return pose;
}
// How often a player's pose is sampled for their skater, from how far they are from both the
// local skater and the camera: every frame within 200 m, 20 Hz past it, 10 Hz past 350 m, and
// every frame again once back inside 190 m. Between samples the skater keeps its pose, which
// saves the interpolation, the pose copy and the skeleton write for players too far away to see.
// Only distance decides. Players out of the camera's view were sampled at 20 Hz as well for a
// while (it saved client-tick time on busy servers), but the game's replays record every skater
// as it stood each frame and their camera looks wherever it likes afterwards: a player who had
// been behind the camera moved at 20 Hz in the replay.
constexpr int far_full_rate_return = 190, far_half_rate_start = 200, far_low_rate_start = 350;
// Metres from a player to the nearer of the local skater and a detached camera (spectating,
// replay or a drone; the follow camera orbits a few metres around the skater, which would
// otherwise shift the distance edge as the camera turns). Infinite if neither is known.
float nearest_distance(const Peer &p, const NativeFrame &local, const std::optional<GameView> &view) {
    const auto &at = p.render_pose.root.position;
    float nearest = std::numeric_limits<float>::infinity();
    const auto consider = [&](float x, float y, float z) {
        const float dx = at[0] - x, dy = at[1] - y, dz = at[2] - z;
        nearest = std::min(nearest, dx * dx + dy * dy + dz * dz);
    };
    if (local.ready) consider(local.pose.root.position[0], local.pose.root.position[1], local.pose.root.position[2]);
    if (view) {
        const float cx = view->world[12], cy = view->world[13], cz = view->world[14];
        if (!local.ready) {
            consider(cx, cy, cz);
        } else {
            const float lx = cx - local.pose.root.position[0];
            const float ly = cy - local.pose.root.position[1];
            const float lz = cz - local.pose.root.position[2];
            if (lx * lx + ly * ly + lz * lz > 12.0f * 12.0f) consider(cx, cy, cz);
        }
    }
    return std::sqrt(nearest);
}
std::uint64_t far_sample_interval(const Peer &p, float distance) {
    const auto beyond = [&](int metres) { return distance > static_cast<float>(metres); };
    if (!std::isfinite(distance) || !beyond(far_full_rate_return)) return 0;
    if (beyond(far_low_rate_start)) return 100000;
    if (beyond(far_half_rate_start) || p.far_interval) return 50000;
    return 0;
}
void render(Session &s, std::uintptr_t client, const NativeFrame &local, std::uint64_t now) {
    if (!world_playing(s, local)) return;
    const auto view = latest_game_view();
    // ReSkate's nametags and chat bubbles: every shown player, placed above their head
    // each frame.
    std::vector<NametagPlayer> nametags;
    const bool bubbles = s.chat_bubbles;
    const bool names = s.nametags && !nametags_held_off.load(std::memory_order_relaxed);
    const bool labels = s.nametags || bubbles; // (published even while names are held off, so none linger)
    if (labels) refresh_friends(s);
    // The newest chat lines from `sender` still inside the bubble duration, oldest first,
    // filtered the same way the chat panel filters them, each with the opacity it has left
    // (it fades over the last half second). At most `chat_bubbles_history` lines.
    const auto recent_bubbles = [&](std::uint64_t sender) {
        std::vector<NametagBubble> result;
        if (!bubbles) return result;
        const int limit = std::max(1, s.chat_bubbles_history);
        const auto duration = static_cast<std::uint64_t>(std::max(0.5f, s.chat_bubbles_duration) * 1e6f);
        const auto fade_from = std::min<std::uint64_t>(duration, 500000);
        for (auto it = s.chat.rbegin(); it != s.chat.rend() && static_cast<int>(result.size()) < limit; ++it) {
            if (it->sender != sender) continue;
            // Older lines are older still: once one has expired, stop.
            if (now < it->received || now - it->received >= duration) break;
            NametagBubble line;
            line.text = it->text;
            if (s.chat_filter) {
                const auto masked = s.chat_masked.find(it->sequence);
                auto filtered = masked != s.chat_masked.end() ? masked->second.second : text::mask_bad_words(it->text);
                if (filtered != it->text) line.raw = std::exchange(line.text, std::move(filtered));
            }
            if (line.text.find_first_not_of(' ') == std::string::npos) continue;
            const auto age = now - it->received;
            // Pops in over the first few frames, fades over the last half second.
            constexpr std::uint64_t pop_us = 220000;
            line.appear = std::min(1.0f, static_cast<float>(age) / static_cast<float>(pop_us));
            line.fade = fade_from ? std::min(1.0f, static_cast<float>(duration - age) / static_cast<float>(fade_from)) : 1.0f;
            result.push_back(std::move(line));
        }
        std::reverse(result.begin(), result.end()); // oldest first
        return result;
    };
    // Grey discs while they typed recently. The step is local, about three a second.
    // ".", "..", "..." is one, two, then three discs; the overlay draws them.
    const auto typing_bubble = [&](std::uint64_t until) {
        NametagBubble line;
        if (!until || now >= until) return line;
        static constexpr const char *steps[] = {".", "..", "..."};
        line.text = steps[(now / 333000) % 3];
        line.hint = true;
        line.appear = line.fade = 1.f;
        return line;
    };
    // Who is talking, as last published for the UI (10 Hz), without copying the voice model.
    const auto &voices = s.view.voice.players;
    const auto label = [&](const Peer &p) {
        NametagPlayer tag;
        tag.head = p.render_pose.root.position;
        tag.head[1] += 1.0f;
        tag.name = p.member.name.empty() ? s.transport.name(p.member.id) : p.member.name;
        // The same role colour and badge the player's chat lines get.
        std::tie(tag.color, tag.tag) = player_role(s, p.member.id, false);
        // Friends only: everyone else keeps their chat bubbles and loses the name and dot.
        tag.nameless = s.nametags_friends && !std::binary_search(s.friend_ids.begin(), s.friend_ids.end(), p.member.id);
        tag.talking = std::any_of(voices.begin(), voices.end(), [&](const auto &v) { return v.id == p.member.id && v.speaking; });
        if (const auto extra = plugin_feed::tag_of(p.member.id)) {
            tag.extra = extra->text;
            tag.extra_color = extra->colour;
        }
        tag.bubbles = recent_bubbles(p.member.id);
        if (bubbles)
            if (auto hint = typing_bubble(p.typing_until); !hint.text.empty()) tag.bubbles.push_back(std::move(hint));
        nametags.push_back(std::move(tag));
    };
    // Creating a player's actor (skater, skateboard and both recipes) or re-applying a
    // changed outfit is one heavy native pass. When several players arrive together
    // (joining a busy server) run one pass per frame; the others wait for later frames.
    bool native_pass{};
    const auto apply_cosmetics = [&](Peer &p, bool spawned) {
        if (now - p.last_cosmetic_apply < 500000) return;
        if (p.applied_cosmetics != p.cosmetic_revision && !spawned) {
            if (native_pass) return;
            native_pass = true;
        }
        update_remote_cosmetics(s.base, local, *p.appearance.value(), p.cosmetic_status);
        // The effects their costume and board items come with follow the outfit.
        if (p.applied_cosmetics != p.cosmetic_revision && s.sync_effects) note_remote_outfit(now);
        p.last_cosmetic_apply = now;
        p.applied_cosmetics = p.cosmetic_revision;
    };
    // The player's sound for this frame, every frame they are shown, and the effects of their
    // skater's contacts that are due.
    const auto present_audio = [&](Peer &p) {
        tick_remote_effects(now);
        if (!s.sync_effects) p.impacts.clear();
        while (!p.impacts.empty() && p.impacts.front().first <= now) {
            const auto &impact = p.impacts.front().second;
            float distance{};
            for (unsigned i = 0; i < 3; ++i) {
                const auto d = impact.position[i] - p.render_pose.root.position[i];
                distance += d * d;
            }
            // Only where their skater is: a contact far from it is not theirs to place.
            if (distance <= 30.f * 30.f) play_impact(impact);
            p.impacts.pop_front();
        }
        if (const auto audio = p.audio.sample(now)) {
            // Preserve discrete audio changes immediately, but let
            // unchanged continuous inputs and spatial transforms
            // ride for one network tick.
            if (!p.presented_audio || audio_event_changed(*p.presented_audio, *audio) ||
                now >= p.next_audio_update) {
                update_remote_audio(s.base, local, p.render_pose, *audio);
                p.presented_audio = *audio;
                p.next_audio_update = now + network_tick_us;
            }
        } else {
            stop_remote_audio();
            p.presented_audio.reset();
            p.next_audio_update = 0;
        }
    };
    // The developer hoodie and board mark an identity: only one Steam vouches for (steam_vouched).
    const auto developer_id = [&](const Peer &p) {
        return steam_vouched(s, p) && shows_items(p) ? p.member.id : std::uint64_t{};
    };
    // Only players within `player_distance` get a skater: each one costs memory and frame time
    // whether or not it is in view, and on a full server most are far across the map, too small
    // to see. Checked twice a second; a player already shown stays until a little further
    // out, so one skating along the edge is not built and taken down over and over (building
    // a skater is the expensive part). Before a pose has placed them, nobody is too far.
    if (now >= s.next_shown_rank) {
        s.next_shown_rank = now + 500000;
        const bool everyone = s.player_distance >= player_distance_unlimited;
        for (auto &p : active_peers(s)) {
            if (!p.member.id || (dedicated_host(s) && p.member.id == s.host_id)) continue;
            p.shown_wanted = everyone || !p.placed ||
                             nearest_distance(p, local, view) <= s.player_distance * (p.visible ? 1.15f : 1.f);
        }
    }
    // Hidden on this screen: by the running throwdown's match, or by a plugin (reskate.session.hide).
    const auto hidden_here = [](std::uint64_t id) { return throwdowns::kit::hidden(id) || plugin_feed::hidden(id); };
    each_active_peer(s, [&](Peer &p) {
        // A dedicated server has no skater to show.
        if (!p.member.id || (dedicated_host(s) && p.member.id == s.host_id))
            return;
        const bool was_visible = p.visible;
        // A far player between samples: their skater keeps its pose; sound, the label and
        // the spectate position are still refreshed (sound events are released one per frame).
        if (was_visible && local.ready && !p.render_failed && p.far_interval && now < p.next_far_sample) {
            update_developer_hoodie(s.base, remote_skater_entity(), developer_id(p), remote_skater_generation(), p.developer_hoodie,
                                    mark_styles(p));
            update_developer_board(s.base, remote_board_entity(), developer_id(p), remote_skater_generation(), p.developer_board,
                                   mark_styles(p));
            present_audio(p);
            update_party_position(hidden_here(p.member.id)?nullptr:&p.render_pose);
            if(hidden_here(p.member.id)) {update_player_ui(s.base,nullptr,{});p.ui_visible=false;}
            if (labels && !hidden_here(p.member.id)) label(p);
            return;
        }
        p.visible = false;
        if (local.ready && !p.render_failed) {
            const bool sampled = s.mode == Mode::echo ? p.poses.sample(now, p.render_pose)
                                                       : p.poses.sample_remote(now, p.render_pose);
            // Nothing new from them for over a second (a stall here, at the host or on the way):
            // their skater stays where it was for a while longer. Taking it down and building
            // it again three seconds later is the most expensive thing a frame can do, and on
            // a busy server one slow frame made it happen to everyone at once, which made
            // the next frames slower still.
            const bool held = !sampled && was_visible && p.poses.heard_within(now, 8000000);
            // Showing a player without an actor spawns one (native_skater_spawn.cpp).
            // Before a pose placed them, shown_wanted stayed true so their status read "Waiting for
            // player poses."; check their distance now that render_pose is known.
            if (sampled && !std::exchange(p.placed, true))
                p.shown_wanted = s.player_distance >= player_distance_unlimited ||
                                 nearest_distance(p, local, view) <= s.player_distance;
            const bool spawning = sampled && p.shown_wanted && p.appearance.value() && !was_visible && !remote_skater_entity();
            if (spawning && now < p.next_spawn) {
                p.native_status = "Waiting to show the player again.";
            } else if (spawning && native_pass) {
                p.native_status = "Waiting for another player's skater to finish spawning.";
            } else if ((sampled || held) && p.shown_wanted && p.appearance.value()) {
                if (spawning) {
                    native_pass = true;
                    p.applied_cosmetics = 0; // the new actor wears no recipe yet
                }
                const bool hidden = throwdown_relay_hides(p.member.id) || hidden_here(p.member.id);
                const auto spot = hidden ? std::nullopt
                                         : throwdown_relay_celebration_offset(p.member.id, p.render_pose.root.position);
                p.visible = show_remote(s.base, client, local,
                                        hidden ? out_of_sight(p.render_pose) : spot ? moved_by(p.render_pose, *spot) : p.render_pose,
                                        p.native_status);
                const auto distance = nearest_distance(p, local, view);
                p.far_interval = p.visible && !hidden ? far_sample_interval(p, distance) : 0;
                p.next_far_sample = now + p.far_interval;
                // The native work a far player's skater may skip (puppet_cost.cpp).
                if (p.visible) note_remote_distance(distance);
                // Solid for the local skater within reach, when both have party collision on.
                // Local Echo shows your own skater: never solid.
                const auto &at = p.render_pose.root.position, &me = local.pose.root.position;
                const float reach2 = (at[0] - me[0]) * (at[0] - me[0]) + (at[1] - me[1]) * (at[1] - me[1]) +
                                     (at[2] - me[2]) * (at[2] - me[2]);
                update_remote_collision(s.base, local.context, p.render_pose,
                                        p.visible && !hidden && s.mode != Mode::echo && s.local_player_collision &&
                                            p.player_collision && reach2 < 40.f * 40.f,
                                        now);
                if (p.visible && hidden) {
                    // Keeps the actor and its cosmetics; no sound, nametag or spectate position.
                    stop_remote_audio();
                    p.presented_audio.reset();
                    if (p.ui_visible) {
                        update_player_ui(s.base, nullptr, {});
                        p.ui_visible = false;
                        p.next_ui_update = 0;
                    }
                    apply_cosmetics(p, spawning);
                } else if (p.visible) {
                    present_audio(p);
                    apply_cosmetics(p, spawning);
                    // The spectate camera follows this every frame; the map and
                    // indicators below only need network-rate updates.
                    update_party_position(hidden_here(p.member.id)?nullptr:&p.render_pose);
                    if (labels && !hidden_here(p.member.id)) label(p);
                    if(hidden_here(p.member.id)) {
                        update_player_ui(s.base,nullptr,{});p.ui_visible=false;
                    } else if (!p.ui_visible || now >= p.next_ui_update) {
                        update_player_ui(s.base, &p.render_pose, p.member.name);
                        p.ui_visible = true;
                        p.next_ui_update = now + network_tick_us;
                    }
                } else {
                    p.render_failed = true;
                    remove_remote(s.base);
                }
            } else {
                if (was_visible) {
                    remove_remote(s.base);
                    p.next_spawn = now + 3000000;
                }
                p.native_status = !p.shown_wanted ? "Further away than players are shown."
                                  : sampled       ? "Waiting for the player's cosmetic recipe."
                                                  : "Waiting for player poses.";
                // No skater for them, but still their name or dot where they are, and where the
                // Social menu's teleport and spectate find them.
                if (!p.shown_wanted && sampled && !hidden_here(p.member.id)) update_party_position(&p.render_pose);
                if (!p.shown_wanted && sampled && labels && !hidden_here(p.member.id)) label(p);
            }
        }
        update_developer_hoodie(s.base, p.visible ? remote_skater_entity() : 0, developer_id(p),
                                 remote_skater_generation(), p.developer_hoodie, mark_styles(p));
        update_developer_board(s.base, p.visible ? remote_board_entity() : 0, developer_id(p),
                               remote_skater_generation(), p.developer_board, mark_styles(p));
        if (!p.visible) {
            stop_remote_audio();
            p.presented_audio.reset();
            p.next_audio_update = 0;
            if (p.ui_visible) {
                update_player_ui(s.base, nullptr, {});
                p.ui_visible = false;
                p.next_ui_update = 0;
            }
        }
    });
    // The local player's own lines, above their own skater, when asked for. The typing
    // preview uses the same three-second window other players see, and shows even when
    // own chat lines are hidden.
    if (labels && bubbles && local.ready) {
        auto own = s.chat_bubbles_own ? recent_bubbles(s.transport.status().local_id) : std::vector<NametagBubble>{};
        if (auto hint = typing_bubble(s.local_typing_until); !hint.text.empty()) own.push_back(std::move(hint));
        if (!own.empty()) {
            NametagPlayer tag;
            tag.head = local.pose.root.position;
            tag.head[1] += 1.0f;
            tag.self = true;
            tag.bubbles = std::move(own);
            nametags.push_back(std::move(tag));
        }
    }
    // Other players' skaters as last heard, for plugins (reskate.session.skaters).
    {
        std::vector<plugin_feed::Peer> peers;
        for (const auto &p : active_peers(s))
            if (p.member.id && p.latest_root)
                peers.push_back({p.member.id, p.latest_root->position, p.latest_root->rotation,
                                 now > p.pose_arrival ? static_cast<double>(now - p.pose_arrival) / 1000.0 : 0.0});
        plugin_feed::publish_peers(std::move(peers));
        // And the joints of theirs that plugins asked for (reskate.entities.set's attach).
        std::map<std::pair<std::uint64_t, std::uint16_t>, plugin_feed::JointPlace> joints;
        if (const auto wanted = plugin_feed::joints_wanted(); !wanted.empty()) {
            std::vector<anim_studio::Joint> pose;
            for (const auto &p : active_peers(s)) {
                if (!p.member.id || p.render_pose.skater.empty()) continue;
                pose.clear();
                for (const auto &[player, joint] : wanted) {
                    if (player != p.member.id || joint >= p.render_pose.skater.size()) continue;
                    if (pose.empty())
                        for (const auto &t : p.render_pose.skater) pose.push_back({t.scale, t.rotation, t.position});
                    const auto w = anim_studio::world(pose, joint);
                    if (anim_studio::finite(w)) joints[{player, joint}] = {w.position, w.rotation, w.scale[0]};
                }
            }
        }
        plugin_feed::publish_joints(std::move(joints));
        // Everyone's body as drawn, while a plugin reads them (touching, reskate.session.raycast). The
        // local skater's skeleton is read only when a pose is sent: between those, its body moves with
        // its root.
        static std::optional<plugin_feed::Body> local_body;
        static std::array<float, 3> local_body_root{};
        if (plugin_feed::bodies_wanted()) {
            std::vector<plugin_feed::Body> bodies;
            std::vector<anim_studio::Joint> pose;
            const auto body = [&](std::uint64_t player, const Pose &from) {
                pose.clear();
                for (const auto &t : from.skater) pose.push_back({t.scale, t.rotation, t.position});
                const auto place = [&](std::uint16_t joint) -> std::optional<std::array<float, 3>> {
                    if (joint >= pose.size()) return std::nullopt;
                    const auto w = anim_studio::world(pose, joint);
                    if (!anim_studio::finite(w)) return std::nullopt;
                    return w.position;
                };
                std::optional<std::pair<std::array<float, 3>, std::array<float, 4>>> board;
                if (!from.board.empty()) board = std::pair{from.board.front().position, from.board.front().rotation};
                return plugin_feed::body_of(player, place, board);
            };
            for (const auto &p : active_peers(s))
                if (p.member.id && p.placed && !p.render_pose.skater.empty() &&
                    !(dedicated_host(s) && p.member.id == s.host_id))
                    bodies.push_back(body(p.member.id, p.render_pose));
            if (!local.ready) local_body.reset();
            else if (!local.pose.skater.empty()) {
                local_body = body(0, local.pose);
                local_body_root = local.pose.root.position;
            }
            if (local_body) {
                auto moved = *local_body;
                for (auto &c : moved.capsules)
                    for (std::size_t i = 0; i < 3; ++i) {
                        const float d = local.pose.root.position[i] - local_body_root[i];
                        c.a[i] += d;
                        c.b[i] += d;
                    }
                bodies.push_back(std::move(moved));
            }
            plugin_feed::publish_bodies(std::move(bodies));
        } else local_body.reset();
    }
    if (labels)
        publish_custom_nametags(s.base, std::move(nametags),
                                local.ready ? std::optional(local.pose.root.position) : std::nullopt,
                                names, bubbles, s.chat_bubbles_distance, s.nametag_distance,
                                s.nametag_dots);
}
} // namespace
void note_player_input() noexcept { note_input(); }
MultiplayerModel model() {
    auto &s = session();
    std::lock_guard lock(s.mutex);
    return s.view;
}
std::string take_leave_notice() { return std::exchange(session().leave_notice, {}); }
std::string take_removed_notice() { return std::exchange(session().removed_notice, {}); }
std::optional<MapNeed> take_map_need() { return std::exchange(session().map_need, std::nullopt); }
void set_map_package_lookup(MapPackageLookup lookup) noexcept { map_package_lookup.store(lookup, std::memory_order_relaxed); }
void set_plugin_check(PluginCheck plugins, LevelCheck levels) noexcept {
    plugin_check.store(plugins, std::memory_order_relaxed);
    level_check.store(levels, std::memory_order_relaxed);
}
bool fetching_map() noexcept { return fetching_map_flag.load(std::memory_order_relaxed); }
std::uint64_t map_fetch_deadline() noexcept { return map_fetch_deadline_flag.load(); }
void hold_map_load(bool hold, std::string_view package) noexcept {
    map_load_held.store(hold && !package.empty() ? map_hash(package) : 0);
}
MultiplayerChat chat() {
    auto &s = session();
    std::lock_guard lock(s.mutex);
    return s.chat_view;
}
std::string plugin_chat(std::string_view plugin, std::string_view text, std::uint32_t text_color) {
    auto &s = session();
    if (s.mode == Mode::off) return "not in a session";
    s.chat.push_back({++s.chat_sequence, 0, now_us(), std::string(plugin), std::string(text), false, 0xffd9a441u, "Plugin", {}, false,
                      text_color});
    while (s.chat.size() > multiplayer_chat_history) s.chat.pop_front();
    publish_chat(s);
    return {};
}
void host_map_change(std::string_view destination) {
    auto &s = session();
    // (Not the host's own reload of the same map: its players stay where they are.)
    if (s.mode != Mode::host || s.local_reload || (!s.started_map && !s.travelling)) return;
    try {
        begin_host_world(s, destination, now_us());
        publish(s);
    } catch (const std::exception &e) {
        stop(s, e.what());
        publish(s);
    }
}
bool begin_local_reload() {
    auto &s = session();
    if (s.mode != Mode::host && s.mode != Mode::join) return true;
    if (!s.started_map || s.travelling || s.awaiting_map || !s.host_world_ready) return false;
    const auto now = now_us();
    s.local_reload = true;
    s.local_reload_started = s.reload_ready_due = false;
    s.travelling = true;
    s.travel_started = now;
    if (s.mode == Mode::host) {
        // The players wait for the host, who says it is not in the world for now.
        s.host_world_ready = false;
        send_world_state(s, now);
        s.status = "Reloading the map for your mods. Your players stay connected.";
    } else {
        s.status = "Reloading the map for your mods. You stay in the session.";
    }
    publish(s);
    return true;
}
namespace {
// The end of a local reload: back in the same world, as it was.
void end_local_reload(Session &s, const NativeFrame &local, std::uint64_t now) {
    s.local_reload = s.local_reload_started = false;
    s.travelling = false;
    s.context = local.context;
    s.parent = local.parent;
    for (auto &peer : active_peers(s)) peer.last_packet = now;
    if (s.mode == Mode::host) {
        s.host_world_ready = true;
        s.last_world_state = 0;
        send_world_state(s, now);
        s.status = "Map reloaded. Your players are still here.";
    } else {
        s.reload_ready_due = true;
        s.status = "Map reloaded. Still in the same session.";
    }
}
}
void cancel_local_reload() {
    auto &s = session();
    if (!s.local_reload || s.local_reload_started) return;
    s.local_reload = false;
    s.travelling = false;
    if (s.mode == Mode::host) {
        s.host_world_ready = true;
        send_world_state(s, now_us());
    }
    s.status.clear();
    publish(s);
}
bool observe_local_world(Session &s, const NativeFrame &local, std::string_view current, std::uint64_t now) {
    const auto map = current.empty() ? 0 : map_hash(current);
    if (s.local_reload) {
        // The map is unloaded and loaded again for the player's own reasons (begin_local_reload): no
        // map change, so nothing of the session changes while it loads.
        if (!s.local_reload_started) {
            if (!local.ready || local.context != s.context) s.local_reload_started = true;
            else if (now - s.travel_started > 20000000) {
                // The load never began (it was refused): the session carries on as it was.
                end_local_reload(s, local, now);
                return true;
            } else
                return true;
        }
        if (!local.ready) {
            if (now - s.travel_started > 180000000) {
                stop(s, "The map took too long to reload.");
                return false;
            }
            return true;
        }
        if (map != s.map) {
            stop(s, "Session ended because your local map changed. Join again to return to the host.");
            return false;
        }
        end_local_reload(s, local, now);
    }
    const bool changed = s.started_map &&
        (!local.ready || s.map != map || s.context != local.context || s.parent != local.parent);
    if (s.mode == Mode::host) {
        if (changed && !s.travelling)
            begin_host_world(s, local.ready ? current : std::string_view{}, now);
        if (s.travelling && local.ready) {
            // A failed native load may leave the old map active. Publish that
            // destination with a fresh generation so guests recover with us.
            if (s.map && s.map != map) begin_host_world(s, current, now);
            s.map = map;
            s.map_name = current;
            s.travelling = false;
            s.host_world_ready = true;
            s.last_world_state = 0;
            for (auto &peer : active_peers(s)) peer.last_packet = now;
            s.status = "Host map loaded. Waiting for players to finish loading...";
        }
    } else if (s.travelling) {
        if (local.ready && !s.awaiting_map && s.host_world_ready) {
            s.travelling = false;
            s.map_fetch_deadline = 0;
            map_fetch_deadline_flag.store(0);
            s.last_world_ready = 0;
            for (auto &peer : active_peers(s)) {
                peer.last_packet = now;
                peer.connected_at = now;
                peer.travel_since = 0;
            }
            s.status = "Map changed. Still connected to the same session.";
        }
    } else if (changed) {
        stop(s, "Session ended because your local map changed. Join again to return to the host.");
        return false;
    }
    if (s.travelling && now - s.travel_started > 180000000) {
        stop(s, "Map change timed out while waiting for loading to finish.");
        return false;
    }
    if (local.ready && !s.awaiting_map) {
        s.started_map = true;
        s.map = map;
        s.map_name = current;
        s.context = local.context;
        s.parent = local.parent;
    }
    return true;
}
// Native loading can unload every local actor. Keep the connection in preflight
// until the selected map and a controllable skater are available.
bool prepare_join_map(Session &s, bool ready, std::string_view current, MapLoader loader, std::uint64_t now) {
    if (map_fetch_policy::expired(s.map_fetch_deadline, now)) {
        stop(s, "Map download timed out. Please try joining the server again.");
        return false;
    }
    if (!s.awaiting_map)
        return true;
    // (While the map is being fetched the clock is held: see below.)
    if (!s.fetching_since && now - s.join_started > 180000000) {
        stop(s, "Joining timed out while waiting for the host's map to load.");
        return false;
    }
    if (!s.join_map_authorized || s.join_destination.empty())
        return false;
    if (map_hash(current) == map_hash(s.join_destination)) {
        if (!ready) {
            s.status = "Host map loaded. Waiting for your skater...";
            return false;
        }
        if (!s.travelling && s.join_started)
            trace::record("multiplayer.join", "app.join", std::chrono::milliseconds((now - s.join_started) / 1000));
        s.awaiting_map = false;
        s.fetching_since = 0;
        fetching_map_flag.store(false, std::memory_order_relaxed);
        s.last_hello = 0;
        s.status = s.travelling ? "Your map is ready. Waiting for the host..." : "Host map ready. Joining the session...";
        return true;
    }
    if (!loader) {
        stop(s, "The native map loader is unavailable. Restart ReSkate and try joining again.");
        return false;
    }
    if (!s.last_map_load_check || now - s.last_map_load_check >= 500000) {
        s.last_map_load_check = now;
        std::string detail;
        // The host's map, installed here at another version: it is not loaded as it is. It is
        // fetched at the host's version as a missing one is (the downloader puts it in the
        // installed one's place), and loads when the two agree.
        const auto have = std::string(other_map_version(s.map_package, host_map_package(s.join_destination)));
        // A dedicated server's required plugins come before its map, which does not load until
        // they are here. The ones missing are fetched as a missing map is, on one card with the
        // map when that is missing too, so the player is asked once.
        if (!s.plugins_ready && !s.required_plugins.empty()) {
            std::vector<std::string> missing = s.required_plugins;
            try {
                if (const auto check = plugin_check.load(std::memory_order_relaxed)) missing = check(s.required_plugins);
            } catch (...) {}
            if (missing.empty()) {
                s.plugins_ready = true;
                // (A fetch of plugins alone is over: the map, which is here, loads as usual.)
                if (s.fetching_since && !s.fetching_with_map) {
                    s.fetching_since = 0;
                    fetching_map_flag.store(false, std::memory_order_relaxed);
                }
            } else {
                if (!s.fetching_since) {
                    // The map goes on the card too when it is a package that no enabled mod here has.
                    const auto levels = level_check.load(std::memory_order_relaxed);
                    bool map_here = s.map_package.empty();
                    try {
                        map_here = map_here || (have.empty() && levels && levels(world_destination_asset(s.join_destination)));
                    } catch (...) {}
                    const auto name = s.map_label.empty() ? world_level_name(world_destination_asset(s.join_destination)) : s.map_label;
                    s.fetching_since = now;
                    s.fetching_with_map = !map_here;
                    if (!s.map_fetch_deadline) s.map_fetch_deadline = now + map_fetch_limit_us;
                    map_fetch_deadline_flag.store(s.map_fetch_deadline);
                    fetching_map_flag.store(true, std::memory_order_relaxed);
                    s.map_need = MapNeed{map_here ? std::string() : s.map_package, name, dedicated_host(s), s.travelling,
                                         map_here ? std::string() : have, s.map_fetch_deadline, std::move(missing)};
                    s.rejoin = Session::Rejoin{{s.host_id, s.secret}, s.password, s.map_fetch_deadline};
                    s.last_map_request = s.last_world_ready = 0; // say so at once
                }
                s.join_started = now;
                s.status = "Waiting for the server's plugins to download...";
                return false;
            }
        }
        // (Nor while its files are being put in place and applied: hold_map_load.)
        const bool held = !s.map_package.empty() && map_load_held.load() == map_hash(s.map_package);
        const auto result = have.empty() && !held ? loader(s.join_destination, s.map_load_submitted, detail) : MapLoadResult::missing;
        if (result == MapLoadResult::missing) {
            const auto name = s.map_label.empty() ? world_level_name(world_destination_asset(s.join_destination)) : s.map_label;
            const char *who = dedicated_host(s) ? "server" : "host";
            const auto where = (s.travelling ? std::string("The ") + who + " moved to " : std::string("The ") + who + " is on ") + name;
            // A map the host named the package of can be downloaded (take_map_need). The
            // session waits for it: the host is told the map is being fetched and gives that
            // far longer than a load, and the map is tried again here until it is installed,
            // when it loads like any other. The wait has an end of its own.
            if (!s.map_package.empty()) {
                if (!s.fetching_since) {
                    s.fetching_since = now;
                    if (!s.map_fetch_deadline) s.map_fetch_deadline = now + map_fetch_limit_us;
                    map_fetch_deadline_flag.store(s.map_fetch_deadline);
                    fetching_map_flag.store(true, std::memory_order_relaxed);
                    s.map_need = MapNeed{s.map_package, name, dedicated_host(s), s.travelling, have, s.map_fetch_deadline};
                    s.rejoin = Session::Rejoin{{s.host_id, s.secret}, s.password, s.map_fetch_deadline};
                    s.last_map_request = s.last_world_ready = 0;   // say so at once
                }
                // The time a load gets starts when the map is here.
                s.join_started = now;
                if (s.travelling) s.travel_started = now;
                s.status = "Waiting for " + name + " to download...";
                return false;
            }
            s.leave_notice = where + ", which is not installed on this PC. Install its map mod and join again.";
            stop(s, s.leave_notice);
            return false;
        }
        if (result == MapLoadResult::failed) {
            stop(s, detail.empty() ? "The host's map could not be loaded." : std::move(detail));
            return false;
        }
        s.map_load_submitted |= result == MapLoadResult::queued;
        s.status = detail.empty() ? "Loading the host's map..." : std::move(detail);
    }
    return false;
}
void tick(std::uintptr_t base, std::uintptr_t client, bool ready, std::string_view map_name, MapLoader loader) {
    auto &s = session();
    struct VoicePlaybackTick {
        VoiceChat &voice;
        std::uintptr_t base;
        ~VoicePlaybackTick() { voice.process_native(base); }
    } voice_playback_tick{s.voice, base};
    s.base = base;
    {
        // (Its hooks, and the throwdown lab's, go in together: one pause of the game's threads.)
        HookBatch batch;
        initialize_native_throwdowns(base);
    }
    profile_runtime::install_script_error_log(base);
    profile_runtime::install_board_wear_hold(base);
    s.gameplay_ready = ready;
    s.available_map = map_name;
    set_multiplayer_session_active(s.mode == Mode::host || s.mode == Mode::join);
    struct PartyPublication {
        Session &value;
        ~PartyPublication() { try { publish_party(value); } catch (...) {} }
    } party_publication{s};
    try {
        std::deque<std::unique_ptr<PrivateRequest>> requests;
        {
            std::lock_guard lock(s.request_mutex);
            requests.swap(s.requests);
        }
        const auto dispatch_now = now_us();
        for (auto &request : requests)
            if (dispatch_now - request->queued <= 10000000)
                command(request->action, request->argument, request->password);
        requests.clear();
        // Commands start asynchronous lobby timers using the current clock.
        s.lobbies.tick(now_us());
        s.servers.tick(now_us());
        if (const auto lobby = s.lobbies.take_join()) {
            PrivateRequest input;
            input.password = s.lobby_password;
            erase_password(s.lobby_password);
            if (s.mode == Mode::off) {
                command("join", lobby->code, input.password);
                if (s.mode == Mode::join) s.joined_public_lobby = lobby->id;
            } else
                s.status = "Lobby join cancelled because the session changed.";
        }
        if (!s.lobbies.status().joining)
            erase_password(s.lobby_password);
        expire_remote_collision(base, now_us());
        if (s.mode == Mode::off) {
            if (!competition_stock_physics()) physics_tuning::release(base);
            set_session_tuning_enforced(false);
            NativeFrame solo;
            if(ready && !map_name.empty()) {
                try {solo=capture_local(base,client,false);if(solo.ready)note_local_skater(solo.pose.root);}
                catch(...) {}
            }
            relay_throwdowns(s, ready, &solo);
            if (competition_stock_physics() && solo.ready) {
                physics_tuning::prepare();physics_tuning::enforce(base,solo.entity,{});
            } else if (!competition_stock_physics()) physics_tuning::release(base);
            if (solo.ready && (throwdowns::kit::match().input_lock || skater_push::holding()))
                prepare_on_board_teleport(client, solo.entity);
            // Without a session the UI model follows at the same 10 Hz as in one;
            // commands still publish at once.
            if (const auto now = now_us(); now >= s.next_publish) {
                s.next_publish = now + 100000;
                publish(s);
            }
            return;
        }
        // Both queued joins and completed browser joins set join_started inside
        // command(). An earlier frame timestamp would underflow timeout checks.
        const auto now = now_us();
        refresh_host_choices(s, now);
        s.client_timing.begin(now);
        struct TimedTick {
            Session &s;
            std::uint64_t started;
            ~TimedTick() {
                if (s.mode == Mode::off) return;
                const auto end = now_us();
                s.client_timing.finish(started, end);
                if (!s.last_client_log) s.last_client_log = end;
                if (end - s.last_client_log < 60000000) return; // a summary a minute, not log spam
                s.last_client_log = end;
                const auto timing = s.client_timing.snapshot(end);
                const auto last_entry = s.client_timing.last_entry;
                s.client_timing = {};
                s.client_timing.last_entry = last_entry;
                logging::log(logging::Level::info, logging::Channel::runtime,
                    "Multiplayer client: callbacks={:.1f}/s, max gap={:.2f} ms, work avg/max={:.2f}/{:.2f} ms, "
                    "capture={:.2f}/{:.2f}, receive={:.2f}/{:.2f}, send={:.2f}/{:.2f}, render={:.2f}/{:.2f}, "
                    "diagnostics={:.2f}/{:.2f} ms (avg/max).",
                    timing.callback_hz, timing.gap_max_ms, timing.work_ms, timing.work_max_ms,
                    timing.mean_ms[0], timing.peak_ms[0], timing.mean_ms[1], timing.peak_ms[1],
                    timing.mean_ms[2], timing.peak_ms[2], timing.mean_ms[3], timing.peak_ms[3],
                    timing.mean_ms[4], timing.peak_ms[4]);
            }
        } timed_tick{s, now};
        NativeFrame local;
        const bool map_ready = prepare_join_map(s, ready, map_name, loader, now);
        // The hooks the remote-player code needs, installed once on the first tick in a world.
        // Each install briefly suspends every game thread; done when the first other player
        // appeared, that was a visible hitch (joining a server with players on it).
        if (ready && !map_name.empty() && !s.hooks_prepared) {
            s.hooks_prepared = true;
            const auto started = now_us();
            std::string entity_detail;
            {
                // All of them in one pause of the game's threads, not one each.
                HookBatch batch;
                prepare_native_indicators(base);
                prepare_player_ui(base);
                if (!install_entity_hooks(base, entity_detail) && !entity_detail.empty())
                    logging::log(logging::Level::warning, logging::Channel::runtime, "Multiplayer: {}", entity_detail);
                prepare_remote_audio(base);
                prepare_effects(base);
            }
            logging::log(logging::Level::info, logging::Channel::runtime,
                         "Multiplayer: remote-player hooks installed in {} ms (once, before anyone joins).",
                         (now_us() - started) / 1000);
        }
        if (s.mode == Mode::off) {
            // Solo, the spectate camera still learns how the game's camera frames the skater.
            if (ready && !map_name.empty()) {
                try {
                    const auto solo = capture_local(base, client, false);
                    if (solo.ready) note_local_skater(solo.pose.root);
                } catch (...) {}
            }
            publish(s);
            return;
        }
        const bool capture_pose = !s.next_send || now >= s.next_send;
        if (map_ready && ready && !map_name.empty())
            local = capture_local(base, client, capture_pose);
        else
            local.detail = s.awaiting_map ? "Waiting for the host's map and local skater." : "Load a map to skate.";
        const auto captured_at = capture_pose ? now_us() : 0;
        if (local.ready) note_local_skater(local.pose.root);
        if (capture_pose && local.ready) {
            s.last_skater_bones = local.pose.skater.size();
            s.last_board_bones = local.pose.board.empty() ? 0 : local.pose.board.size() - 1;
        }
        if (!observe_local_world(s, local, map_name, now)) {
            s.voice.reset();
            publish(s);
            return;
        }
        if (!local.ready) {
            s.native_status = local.detail;
        } else {
            if (world_playing(s, local)) prepare_audio_capture(base, local);
        }
        const auto receive_at = now_us();
        s.client_timing.record(ClientTiming::capture, now, receive_at);
        if (s.mode != Mode::echo)
            networking(s, local, receive_at);
        for (const auto &note : s.transport.take_direct_notes())
            logging::log(logging::Level::info, logging::Channel::runtime, "Multiplayer: {}", note);
        // A menu or respawn can suspend the pose capture while this world and
        // its Throwdown queue remain loaded. Do not cancel their coordinators.
        relay_throwdowns(s, s.gameplay_ready && !s.travelling && !s.awaiting_map && s.host_world_ready);
        // A S.K.A.T.E. player waiting for their turn stays off the board.
        update_board_lock(client, local.entity, local.ready && (throwdown_relay_waits_offboard() || throwdowns::kit::match().offboard));
        if (local.ready && (throwdowns::kit::match().input_lock || skater_push::holding()))
            prepare_on_board_teleport(client, local.entity);
        update_physics_tuning(s, local, now);
        const auto send_at = now_us();
        s.client_timing.record(ClientTiming::network, receive_at, send_at);
        if (s.mode != Mode::off) {
            VoiceScene voice_scene;
            voice_scene.active = s.mode != Mode::echo && world_playing(s, local);
            voice_scene.policy = s.voice_policy;
            if (s.mode == Mode::join && !s.roster_sequence) voice_scene.policy.allowed = false;
            voice_scene.session = s.secret; voice_scene.world = s.world;
            voice_scene.listener = local.pose.root;
            std::array<float, 16> camera{};
            if (voice_scene.active && voice_scene.policy.allowed && s.voice_settings.enabled &&
                read_local_camera_transform(base, client, camera)) {
                voice_scene.ear_position = {camera[12], camera[13], camera[14]};
                voice_scene.ear_right = {camera[0], camera[1], camera[2]};
            }
            for (const auto &peer : active_peers(s))
                if (peer.handshaken && peer.latest_root && peer.world_ready && peer.pose_arrival &&
                    send_at - peer.pose_arrival <= 1000000)
                    voice_scene.peers.push_back({peer.member.id, peer.member.epoch, *peer.latest_root});
            s.voice.update(std::move(voice_scene));
            for (auto &data : s.voice.take_capture()) {
                auto voice = packet(s, PacketKind::voice, send_at);
                voice.voice = std::move(data);
                broadcast(s, voice, false, true, send_at);
            }
            sync_objects(s, local, send_at);
            send_local(s, local, send_at, captured_at, capture_pose);
            const auto render_at = now_us();
            s.client_timing.record(ClientTiming::send, send_at, render_at);
            render(s, client, local, render_at);
            const auto diagnostics_at = now_us();
            s.client_timing.record(ClientTiming::render, render_at, diagnostics_at);
            // Native rendering/networking remain per-frame. Rebuild the UI
            // snapshot at 10 Hz; explicit commands still publish immediately.
            if (diagnostics_at >= s.next_publish) {
                s.next_publish = diagnostics_at + 100000;
                publish(s, &local);
            }
            s.client_timing.record(ClientTiming::publish, diagnostics_at, now_us());
        } else {
            publish(s, &local);
        }
    } catch (const std::exception &e) {
        stop(s, std::string("Multiplayer stopped: ") + e.what());
        publish(s);
    }
}
} // namespace dingosdk::multiplayer
