#include "session_internal.h"
#include "Engine/Game/World/world_names.h"
#include "Extension/Multiplayer/Hud/native_player_ui.h"
#include "Extension/Multiplayer/Remote/native_audio.h"
#include "Extension/Multiplayer/Steam/steam_social.h"
#include "Extension/Multiplayer/Hud/custom_nametags.h"
#include "Extension/Multiplayer/Hud/game_ui_state.h"
#include "Extension/Profile/local_profile_runtime.h"
#include "Extension/Objects/network_object_runtime.h"
#include "Engine/Core/Log/logging.h"
#include "Engine/Core/Text/word_filter.h"
#include "Extension/Skater/physics_tuning.h"
#include <cmath>
#include <utility>
#include <algorithm>
#include <charconv>

namespace dingosdk::multiplayer::session_detail {
// ---- saved host settings ("ReSkate.Host.*" in the local profile)
void load_host_preferences(Session &s) {
    auto &p = s.host_preferences;
    if (p.loaded) return;
    p.loaded = true;
    const auto number = [](const char *key) -> std::optional<std::int64_t> {
        const auto value = profile_runtime::local_value(key);
        if (!value || !value->is_number()) return {};
        const auto result = value->get<double>();
        return std::isfinite(result) && std::abs(result) < 1e9 ? std::optional<std::int64_t>(static_cast<std::int64_t>(result)) : std::nullopt;
    };
    p.public_lobby = profile_runtime::local_preference("Host.Public").value_or(true);
    p.password_required = profile_runtime::local_preference("Host.RequirePassword").value_or(false);
    p.world_layer_sync = profile_runtime::local_preference("Host.WorldLayerSync").value_or(false);
    // A limit saved before lobbies were capped comes back as the cap.
    if (const auto value = number("Host.Capacity"); value && *value >= 2 && *value <= static_cast<std::int64_t>(max_players))
        p.capacity = static_cast<unsigned>(std::min<std::int64_t>(*value, multiplayer_lobby_player_limit));
    if (const auto value = number("Host.Tps"); value && *value > 0 && valid_multiplayer_tps(static_cast<unsigned>(*value)))
        p.tps = static_cast<unsigned>(*value);
    if (const auto value = profile_runtime::local_value("Host.LobbyName"); value && value->is_string() &&
        value->string().size() <= 128)
        p.lobby_name = value->string();
    MultiplayerDistances distances;
    const std::array<std::pair<const char *, int *>, 4> fields{{
        {"Host.Distance.FullReturn", &distances.full_rate_return}, {"Host.Distance.HalfStart", &distances.half_rate_start},
        {"Host.Distance.HalfReturn", &distances.half_rate_return}, {"Host.Distance.LowStart", &distances.low_rate_start}}};
    for (const auto &[key, field] : fields)
        if (const auto value = number(key)) *field = static_cast<int>(*value);
    if (distances.valid()) p.distances = distances;
    if (const auto value = profile_runtime::local_value("Host.VoiceRange"); value && value->is_number() &&
        valid_voice_range(value->get<float>()))
        p.voice_range = value->get<float>();
    if (const auto value = number("Host.ObjectPlacement"); value && *value >= 0 && valid_object_placement(static_cast<std::uint64_t>(*value)))
        p.placement = static_cast<ObjectPlacement>(*value);
    p.guest_noclip = profile_runtime::local_preference("Host.GuestNoclip").value_or(true);
    p.guest_no_bail = profile_runtime::local_preference("Host.GuestNoBail").value_or(true);
    p.guest_boosts = profile_runtime::local_preference("Host.GuestBoosts").value_or(true);
    p.enforce_tuning = profile_runtime::local_preference("Host.EnforceTuning").value_or(true);
    p.score_check = profile_runtime::local_preference("Host.ScoreCheck").value_or(true);
}
void save_host_preferences(const Session &s) {
    const auto &p = s.host_preferences;
    profile_runtime::set_local_values({
        {"Host.Public", p.public_lobby},
        {"Host.RequirePassword", p.password_required},
        {"Host.WorldLayerSync", p.world_layer_sync},
        {"Host.Capacity", static_cast<std::int64_t>(p.capacity)},
        {"Host.Tps", static_cast<std::int64_t>(p.tps)},
        {"Host.LobbyName", p.lobby_name},
        {"Host.Distance.FullReturn", static_cast<std::int64_t>(p.distances.full_rate_return)},
        {"Host.Distance.HalfStart", static_cast<std::int64_t>(p.distances.half_rate_start)},
        {"Host.Distance.HalfReturn", static_cast<std::int64_t>(p.distances.half_rate_return)},
        {"Host.Distance.LowStart", static_cast<std::int64_t>(p.distances.low_rate_start)},
        {"Host.ObjectPlacement", static_cast<std::int64_t>(p.placement)},
        {"Host.GuestNoclip", p.guest_noclip},
        {"Host.GuestNoBail", p.guest_no_bail},
        {"Host.GuestBoosts", p.guest_boosts},
        {"Host.EnforceTuning", p.enforce_tuning},
        {"Host.ScoreCheck", p.score_check},
        {"Host.VoiceRange", static_cast<double>(p.voice_range)}});
}
// Bans live in the local profile as [{"id": "<SteamID64>", "name": ..., "added": <unix>}];
// the ID is a string because a JSON number cannot hold every 64-bit value.
void load_bans(Session &s) {
    if (s.bans_loaded) return;
    s.bans_loaded = true;
    s.bans.clear();
    s.ban_ids_dirty = true;
    const auto value = profile_runtime::local_value("Host.Bans");
    if (!value || !value->is_array()) return;
    for (const auto &entry : *value) {
        if (!entry.is_object() || !entry.contains("id") || !entry.at("id").is_string()) continue;
        MultiplayerBan ban;
        const auto &text = entry.at("id").string();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ban.id);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !ban.id) continue;
        if (entry.contains("name") && entry.at("name").is_string() && entry.at("name").string().size() <= 128)
            ban.name = entry.at("name").string();
        if (entry.contains("added") && entry.at("added").is_number())
            ban.added = entry.at("added").get<std::int64_t>();
        if (std::none_of(s.bans.begin(), s.bans.end(), [&](const auto &b) { return b.id == ban.id; }))
            s.bans.push_back(std::move(ban));
    }
}
void save_bans(const Session &s) {
    Json list = Json::array();
    for (const auto &ban : s.bans) {
        auto item = Json::object();
        item["id"] = std::to_string(ban.id);
        item["name"] = ban.name;
        item["added"] = ban.added;
        list.push_back(std::move(item));
    }
    profile_runtime::set_local_values({{"Host.Bans", std::move(list)}});
}
bool is_banned(Session &s, std::uint64_t id) {
    load_bans(s);
    // The host checks every connection each frame; keep the IDs sorted for that.
    if (s.ban_ids_dirty) {
        s.ban_ids_dirty = false;
        s.ban_ids.clear();
        for (const auto &ban : s.bans) s.ban_ids.push_back(ban.id);
        std::sort(s.ban_ids.begin(), s.ban_ids.end());
    }
    return std::binary_search(s.ban_ids.begin(), s.ban_ids.end(), id);
}
void refresh_friends(Session &s) {
    const auto social = steam_social_snapshot();
    if (!social || (s.friend_revision && *s.friend_revision == social->revision)) return;
    s.friend_revision = social->revision;
    s.friend_ids.clear();
    for (const auto &f : social->friends) s.friend_ids.push_back(f.id);
    std::sort(s.friend_ids.begin(), s.friend_ids.end());
}
void publish(Session &s, const NativeFrame *local) {
    // Chat stays off screen in the main and pause menus and while the game hides its UI, as
    // the nametags do.
    const auto ui = sample_game_ui_state(s.base);
    s.game_menu = ui.in_menu || ui.ui_hidden;
    MultiplayerModel view;
    load_bans(s);
    view.bans = s.bans;
    view.voice = s.voice.model();
    view.voice.allowed = s.voice_policy.allowed && (s.mode != Mode::join || s.roster_sequence != 0);
    if (!view.voice.allowed) {
        view.voice.transmitting = view.voice.ready = false;
        view.voice.status = s.mode == Mode::join && !s.roster_sequence ? "Waiting for the host's voice policy." : "Voice chat is disabled by the host.";
    }
    view.tps = s.tps;
    view.distances = s.distances;
    view.object_placement = s.object_placement;
    view.guest_noclip = s.guest_noclip;
    view.guest_no_bail = s.guest_no_bail;
    view.guest_boosts = s.guest_boosts;
    view.enforce_tuning = s.enforce_tuning;
    view.tuning_status = s.mode == Mode::join && s.enforce_tuning ? physics_tuning::status() : std::string{};
    view.force_world_layers = s.force_world_layers;
    view.host_id = s.host_id;
    const auto now = now_us();
    view.client_timing = s.client_timing.snapshot(now);
    const auto &t = s.transport.status();
    view.password_required = s.password.has_value();
    view.direct_upload_limit = s.direct_upload.limit;
    view.active = s.mode != Mode::off;
    view.hosting = s.mode == Mode::host;
    view.echo = s.mode == Mode::echo;
    view.local_id = t.local_id;
    view.local_name = s.mode != Mode::off ? s.transport.name(t.local_id) : steam_social_snapshot()->local.name;
    view.lobby_name = s.lobby_name;
    // A dedicated server's roster capacity counts the server itself.
    view.capacity = static_cast<int>(dedicated_host(s) ? s.capacity - 1 : s.capacity);
    load_host_preferences(s);
    view.saved_host = {true, s.host_preferences.public_lobby, s.host_preferences.password_required,
                       static_cast<int>(s.host_preferences.capacity), s.host_preferences.tps, s.host_preferences.lobby_name};
    view.nametags = s.nametags;
    view.custom_nametags = s.custom_nametags;
    if (const auto social = steam_social_snapshot())
        if (const auto mark = identity_mark(social->local.id)) {
            std::tie(view.identity_tag_colour, view.identity_tag) = mark_role(*mark);
            view.identity_animation =
                *mark == IdentityList::developer ? "RAINBOW" : *mark == IdentityList::content_creator ? "RED" : "GOLD";
            const auto styles = developer_hoodie_detail::own_styles.load();
            const auto standard = developer_hoodie_detail::standard_picks(*mark);
            view.identity_styles.resize(styles.size());
            for (std::size_t i = 0; i < styles.size(); ++i) {
                // A cosmetic never given colours shows its list's own in the pickers.
                const bool picked = styles[i].mode == MarkMode::gradient || styles[i].mode == MarkMode::solid ||
                                    styles[i].from != styles[i].to || styles[i].from != std::array<std::uint8_t, 3>{};
                const auto &from = picked ? styles[i].from : standard.first, &to = picked ? styles[i].to : standard.second;
                auto &shown = view.identity_styles[i];
                shown.name = mark_item_names[i];
                shown.mode = static_cast<int>(styles[i].mode);
                shown.speed = styles[i].speed;
                for (std::size_t part = 0; part < 3; ++part)
                    shown.from[part] = static_cast<float>(from[part]) / 255.f, shown.to[part] = static_cast<float>(to[part]) / 255.f;
            }
        }
    view.identity_tag_shown = own_tag_shown();
    view.identity_items_shown = own_items_shown();
    load_host_preferences(s);
    view.voice_range = s.mode == Mode::host ? s.voice_range
                     : dedicated_host(s) ? s.roster_voice_range : s.host_preferences.voice_range;
    view.dedicated = dedicated_host(s);
    view.server_admin = view.dedicated && s.server_admin;
    view.party = s.local_party;
    view.party_leader = s.local_party_leader;
    view.party_open = s.local_party_open;
    view.parties = s.mode == Mode::host || s.mode == Mode::join;
    for (const auto &invite : s.party_invites) {
        const auto *from = find_peer(s, invite.from);
        view.party_invites.push_back({invite.from, from && !from->member.name.empty() ? from->member.name
                                                                                     : s.transport.name(invite.from)});
    }
    if (view.server_admin) {
        view.bans = s.server_bans;
        view.server_bans = true;
        view.server_ban_total = s.server_ban_total;
        view.server_maps = s.server_maps;
        view.server_map_pool = s.server_map_pool;
        view.server_map_rotation = s.server_map_rotation;
        view.server_map_votes = (s.server_votes & server_vote_map) != 0;
    }
    view.chat_visible = s.chat_visible;
    view.chat_filter = s.chat_filter;
    view.chat_bubbles = s.chat_bubbles;
    view.chat_bubbles_own = s.chat_bubbles_own;
    view.chat_bubbles_distance = s.chat_bubbles_distance;
    view.chat_bubbles_duration = s.chat_bubbles_duration;
    view.chat_bubbles_history = s.chat_bubbles_history;
    view.object_status = network_object_status();
    view.players = static_cast<int>(player_count(s));
    view.sent = t.sent;
    view.received = t.received;
    view.dropped = t.dropped;
    view.network_telemetry = t.telemetry;
    view.ping_ms = t.ping_ms;
    view.send_rate = t.send_rate;
    view.pending_bytes = t.pending_bytes;
    view.outgoing_bps = t.outgoing_bps;
    view.incoming_bps = t.incoming_bps;
    view.delivery_local = t.delivery_local;
    view.delivery_remote = t.delivery_remote;
    view.queue_us = t.queue_us;
    view.prioritized_connections = t.prioritized_connections;
    view.cosmetic_queue_us = t.cosmetic_queue_us;
    view.skipped_updates = t.skipped;
    view.send_failures = t.send_failures;
    view.invalid_messages = t.invalid_messages;
    view.sent_bytes = t.sent_bytes;
    view.raw_sent_bytes = t.raw_sent_bytes;
    view.invite = s.invite;
    view.map = s.map_name.empty() ? s.available_map : s.map_name;
    view.local_ready = s.gameplay_ready && !s.available_map.empty();
    const auto &lobby = s.lobbies.status();
    view.public_host = s.public_host;
    view.public_lobby = s.mode == Mode::host ? s.lobbies.hosted_lobby() : s.joined_public_lobby;
    view.lobby_listed = lobby.listed;
    view.lobby_searching = lobby.searching || s.servers.searching();
    view.lobby_joining = lobby.joining;
    view.lobby_searched = lobby.searched;
    view.lobby_status = lobby.hosting;
    view.browser_status = lobby.browser;
    // Dedicated servers first: they are always up and never a stranger's own session.
    view.lobbies = s.servers.rows();
    view.lobbies.insert(view.lobbies.end(), lobby.rows.begin(), lobby.rows.end());
    view.status = s.status;
    view.native_status = s.native_status;
    view.audio_captured = captured_audio_frames();
    each_peer([&] {
        auto &p = s.peers[peer_slot];
        const auto native_count = remote_pose_updates(), board_count = remote_board_pose_updates();
        const auto animation = remote_animation_stats();
        view.pose_updates += native_count;
        view.board_pose_updates += board_count;
        view.audio_played += played_audio_frames();
        view.player_map_updates += player_map_updates();
        if (!p.member.id)
            return;
        view.connected |= p.handshaken;
        if (dedicated_host(s) && p.member.id == s.host_id) {
            // The server is not a player: name the session after it instead.
            view.lobby_name = p.member.name;
            return;
        }
        view.remote_visible |= p.visible;
        const auto name = p.member.name.empty() ? std::to_string(p.member.id) : p.member.name;
        view.roster.push_back({p.member.id,
                               name,
                               p.handshaken,
                               p.visible,
                               p.native_status,
                               p.cosmetic_status,
                               native_audio_status(),
                               player_ui_status(),
                               {},
                               0,
                               0});
        view.roster.back().epoch = p.member.epoch;
        view.roster.back().party = p.member.party;
        view.roster.back().party_leader = p.member.party_leader;
        view.roster.back().party_member = party_member(s, p.member.id);
        if (!p.rate_at) {
            p.rate_at = now;
            p.native_rate_count = native_count;
            p.board_rate_count = board_count;
            p.animation_rate_base = animation;
        }
        if (now - p.rate_at >= 1000000) {
            p.pose_hz = static_cast<float>(p.pose_count - p.rate_count) * 1000000.f /
                        static_cast<float>(now - p.rate_at);
            const auto rate = 1000000.f / static_cast<float>(now - p.rate_at);
            p.native_pose_hz = static_cast<float>(native_count - std::min(native_count, p.native_rate_count)) * rate;
            p.native_board_hz = static_cast<float>(board_count - std::min(board_count, p.board_rate_count)) * rate;
            const auto evaluations = animation.evaluated -
                std::min(animation.evaluated, p.animation_rate_base.evaluated);
            const auto applies = animation.evaluated + animation.skipped -
                std::min(animation.evaluated + animation.skipped,
                         p.animation_rate_base.evaluated + p.animation_rate_base.skipped);
            const auto evaluation_us = animation.evaluation_us -
                std::min(animation.evaluation_us, p.animation_rate_base.evaluation_us);
            const auto apply_us = animation.apply_us -
                std::min(animation.apply_us, p.animation_rate_base.apply_us);
            p.native_animation_hz = static_cast<float>(evaluations) * rate;
            p.native_animation_ms = evaluations ? static_cast<float>(evaluation_us) / (1000.f * evaluations) : 0.f;
            p.pose_apply_ms = applies ? static_cast<float>(apply_us) / (1000.f * applies) : 0.f;
            p.native_rate_count = native_count;
            p.board_rate_count = board_count;
            p.animation_rate_base = animation;
            p.rate_at = now;
            p.rate_count = p.pose_count;
        }
        auto &row = view.roster.back();
        row.pose_hz = p.pose_hz;
        row.native_pose_hz = p.native_pose_hz;
        row.native_board_hz = p.native_board_hz;
        row.native_animation_hz = p.native_animation_hz;
        row.native_animation_ms = p.native_animation_ms;
        row.pose_apply_ms = p.pose_apply_ms;
        row.native_animation_skipped = animation.skipped;
        const auto playback = p.poses.playback();
        row.playback = s.mode == Mode::echo ? "Buffered echo" :
            playback.mode == PosePlaybackMode::buffered ? "Buffered" :
            playback.mode == PosePlaybackMode::predicted ? "Predicting" :
            playback.mode == PosePlaybackMode::held ? "Holding" : "Waiting";
        row.prediction_ms = static_cast<unsigned>(playback.prediction_us / 1000);
        row.correcting = playback.correcting;
        row.pose_target_tps = 1000000 / p.received_pose_interval;
        if (p.latest_root && s.local_root) {
            float distance{};
            for (unsigned i = 0; i < 3; ++i) {
                const auto d = p.latest_root->position[i] - s.local_root->position[i];
                distance += d * d;
            }
            row.distance_m = std::sqrt(distance);
        }
        row.pose_age_ms = p.pose_arrival ? (now - p.pose_arrival) / 1000 : 0;
        const bool direct = s.mode == Mode::host || p.member.id == s.host_id ||
                            (p.direct_ready && p.last_direct_pose && now - p.last_direct_pose <= 500000);
        row.route = s.mode == Mode::echo ? "Local Echo" : direct ? "Steam peer" : "Via host";
        if (s.mode != Mode::echo && p.handshaken) {
            if (direct)
                ++view.direct_connections;
            else
                ++view.fallback_streams;
            if (s.mode == Mode::host)
                for (const auto &other : active_peers(s))
                    if (other.handshaken && other.member.id != p.member.id &&
                        needs_relay(p.direct_routes, p.route_reported, other.member, now))
                        ++view.fallback_streams;
        }
        if (!view.peer_id) {
            view.peer_id = p.member.id;
            view.peer_name = name;
            view.native_status = p.native_status;
            view.cosmetic_status = p.cosmetic_status;
            view.audio_status = row.audio_status; // this slot's, read above
            view.player_ui_status = row.ui_status;
        }
    });
    if (!s.cosmetic_capture_status.empty())
        view.cosmetic_status = s.cosmetic_capture_status;
    view.lobby_joining = lobby.joining || (s.mode == Mode::join && !view.connected);
    if (local) {
        view.local_ready = local->ready;
    }
    view.skater_bones = s.last_skater_bones;
    view.board_bones = s.last_board_bones;
    publish_chat(s); // takes the same lock, so before it
    // Only this thread writes s.view, so it reads it without the lock. Log before taking
    // the lock the overlay copies the model under: file and console writes never hold it.
    if (view.status != s.view.status || view.players != s.view.players)
        logging::log(logging::Level::info, logging::Channel::runtime, "Multiplayer: {} Players {}/{}",
                     view.status, view.players, view.capacity);
    if (view.lobby_status != s.view.lobby_status && !view.lobby_status.empty())
        logging::log(logging::Level::info, logging::Channel::runtime, "Multiplayer: {}", view.lobby_status);
    if (view.connected && now - s.last_network_log >= 60000000) { // a summary a minute, not log spam
        const auto local_hz = s.last_network_log
            ? static_cast<double>(s.local_pose_count - s.logged_local_pose_count) * 1000000. /
                static_cast<double>(now - s.last_network_log) : 0.;
        s.logged_local_pose_count = s.local_pose_count;
        s.last_network_log = now;
        logging::log(logging::Level::info, logging::Channel::runtime,
                     "Multiplayer network: players={}, direct={}, relay streams={}, out={:.1f} KiB/s, "
                     "in={:.1f} KiB/s, queue={:.1f} ms, ping={} ms, local poses={:.1f}/s, "
                     "direct budget={}, skipped={}, dropped={}.",
                     view.players, view.direct_connections, view.fallback_streams, view.outgoing_bps / 1024.f,
                     view.incoming_bps / 1024.f, static_cast<double>(view.queue_us) / 1000., view.ping_ms,
                     local_hz, view.direct_upload_limit, view.skipped_updates, view.dropped);
        // One line per player every 5 s adds up in a full lobby: debug level (off by default).
        if (logging::enabled(logging::Level::debug))
            for (const auto &row : view.roster)
                logging::log(logging::Level::debug, logging::Channel::runtime,
                             "Multiplayer delivery: peer={}, route={}, poses={:.1f}/s, target={} TPS, newest arrival={} ms ago, "
                             "native skater/board={:.1f}/{:.1f}/s, graph={:.1f}/s ({:.3f} ms), "
                             "pose apply={:.3f} ms, graph skips={}, playback={}, prediction={} ms, correcting={}.",
                             row.id, row.route, row.pose_hz, row.pose_target_tps, row.pose_age_ms,
                             row.native_pose_hz, row.native_board_hz, row.native_animation_hz,
                             row.native_animation_ms, row.pose_apply_ms, row.native_animation_skipped,
                             row.playback, row.prediction_ms, row.correcting);
    }
    std::lock_guard lock(s.mutex);
    s.view = std::move(view);
}
std::vector<MultiplayerChatCommand> chat_commands(const Session &s) {
    std::vector<MultiplayerChatCommand> list{
        {"/help", "/help", "List these commands"},
        {"/tp", "/tp <player>", "Teleport beside a player (or /tp <x> <y> <z>)", "player"},
        {"/p", "/p <message>", "Talk to your party only"},
    };
    if (s.mode == Mode::host || s.mode == Mode::join) {
        list.push_back({"/party invite", "/party invite <player>", "Invite a player to your party", "player"});
        list.push_back({"/party accept", "/party accept [player]", "Join the party you were invited to", "player"});
        list.push_back({"/party decline", "/party decline [player]", "Turn down a party invite", "player"});
        list.push_back({"/party join", "/party join <player>", "Join a player's open party", "player"});
        list.push_back({"/party leave", "/party leave", "Leave your party"});
        list.push_back({"/party kick", "/party kick <player>", "Leader: remove a player from the party", "player"});
        list.push_back({"/party promote", "/party promote <player>", "Leader: hand the lead to a player", "player"});
        list.push_back({"/party open", "/party open", "Leader: let anyone join the party (/party close: invite only)"});
        list.push_back({"/party", "/party", "Who is in your party"});
    }
    if (dedicated_host(s)) {
        if (s.server_votes & server_vote_map) list.push_back({"/vote map", "/vote map <map>", "Start a vote to change the map", "map"});
        if (s.server_votes & server_vote_kick)
            list.push_back({"/vote kick", "/vote kick <player>", "Start a vote to kick a player", "player"});
        if (s.server_votes & server_vote_time)
            list.push_back({"/vote tod", "/vote tod <time>", "Vote for a time of day: morning, noon, afternoon, evening, night...", "time"});
        if (s.server_votes) {
            list.push_back({"/yes", "/yes", "Vote yes in the running vote"});
            list.push_back({"/no", "/no", "Vote no in the running vote"});
        }
        if (s.server_admin) {
            list.push_back({"/kick", "/kick <player>", "Admin: kick a player until the server restarts", "player"});
            list.push_back({"/ban", "/ban <player>", "Admin: ban a player", "player"});
            list.push_back({"/map", "/map <map>", "Admin: change the server's map", "map"});
            list.push_back({"/tpall", "/tpall [player]", "Admin: teleport everyone to you (or to a player)", "player"});
            list.push_back({"/tphere", "/tphere <player>", "Admin: teleport a player to you", "player"});
            list.push_back({"/tod", "/tod <time>", "Admin: set the time of day", "time"});
            list.push_back({"/votes", "/votes [map|kick|tod on|off|<percent>]", "Admin: the server's vote settings"});
            list.push_back({"/vote-cancel", "/vote-cancel", "Admin: stop the running vote"});
            list.push_back({"/map-pool", "/map-pool [add|remove <map>|clear]", "Admin: the maps players vote between and the rotation uses"});
            list.push_back({"/rotation", "/rotation [<minutes>|off]", "Admin: change the map on a timer, through the map pool"});
        }
    }
    return list;
}
namespace {
// FNV-1a over the values a view is built from, to skip rebuilding an unchanged one.
struct Signature {
    std::uint64_t value = 14695981039346656037ULL;
    void add(std::uint64_t number) {
        for (unsigned i = 0; i < 8; ++i)
            value = (value ^ ((number >> (8 * i)) & 0xff)) * 1099511628211ULL;
    }
    void add(std::string_view text) {
        for (const unsigned char c : text)
            value = (value ^ c) * 1099511628211ULL;
        add(static_cast<std::uint64_t>(text.size()));
    }
};
} // namespace
void publish_chat(Session &s) {
    const auto local = s.transport.status().local_id;
    const bool dedicated = dedicated_host(s);
    const auto listed = [&](const Peer &peer) {
        return peer.handshaken && peer.member.id && peer.member.id != local && !(dedicated && peer.member.id == s.host_id);
    };
    // publish() calls this at 10 Hz, rosters and chat lines too. Rebuilding copies the 50
    // lines and sorts names, so first check whether anything the view shows changed:
    // the "/" list (mode, votes, admin), the players, the server's maps, visibility and
    // the lines (added and dropped only, with increasing sequences).
    Signature signature;
    signature.add(static_cast<std::uint64_t>(s.mode));
    signature.add(static_cast<std::uint64_t>(dedicated));
    signature.add(static_cast<std::uint64_t>(s.server_votes));
    signature.add(static_cast<std::uint64_t>(s.server_admin));
    signature.add(static_cast<std::uint64_t>(s.chat_visible));
    signature.add(static_cast<std::uint64_t>(s.chat_filter));
    signature.add(static_cast<std::uint64_t>(s.game_menu));
    for (const auto &peer : active_peers(s))
        if (listed(peer)) signature.add(peer.member.name.empty() ? s.transport.name(peer.member.id) : peer.member.name);
    for (const auto &asset : s.server_maps) signature.add(asset);
    signature.add(static_cast<std::uint64_t>(s.chat.size()));
    if (!s.chat.empty()) {
        signature.add(s.chat.front().sequence);
        signature.add(s.chat.back().sequence);
    }
    if (s.chat_signature == signature.value) return;
    s.chat_signature = signature.value;
    MultiplayerChat view;
    view.commands = chat_commands(s);
    // Argument completion: the other players, and the dedicated server's maps.
    for (const auto &peer : active_peers(s))
        if (listed(peer))
            view.players.push_back(peer.member.name.empty() ? s.transport.name(peer.member.id) : peer.member.name);
    for (const auto &asset : s.server_maps) view.maps.push_back(world_level_name(asset));
    std::sort(view.players.begin(), view.players.end());
    std::sort(view.maps.begin(), view.maps.end());
    view.maps.erase(std::unique(view.maps.begin(), view.maps.end()), view.maps.end());
    // Hidden chat is not offered at all: no lines on screen and T does nothing. A game menu
    // (or the game's hidden UI) also closes an open chat box.
    view.available = (s.mode == Mode::host || s.mode == Mode::join) && s.chat_visible && !s.game_menu;
    view.latest = s.chat.empty() ? 0 : s.chat.back().sequence;
    view.lines.assign(s.chat.begin(), s.chat.end());
    // The filter masks each line once; lines leave the cache with the log.
    if (!s.chat_filter || s.chat.empty()) {
        s.chat_masked.clear();
    } else {
        s.chat_masked.erase(s.chat_masked.begin(), s.chat_masked.lower_bound(s.chat.front().sequence));
        for (auto &line : view.lines) {
            auto [found, added] = s.chat_masked.try_emplace(line.sequence);
            if (added) found->second = {text::mask_bad_words(line.name), text::mask_bad_words(line.text)};
            line.name = found->second.first;
            if (found->second.second != line.text) line.unmasked = std::exchange(line.text, found->second.second);
        }
    }
    std::lock_guard lock(s.mutex);
    s.chat_view = std::move(view);
}
std::pair<std::uint32_t, std::string> mark_role(IdentityList list) {
    switch (list) {
    case IdentityList::developer: return {nametag_developer, "Dev"};
    case IdentityList::content_creator: return {nametag_creator, "Creator"};
    default: return {nametag_homie, "Homie"};
    }
}
// The colour and tag a player gets, in chat and on their nametag. `marks` off leaves out who
// the backend says they are: for a chat line that is not known to be theirs (chat proofs,
// session_receive.cpp).
std::pair<std::uint32_t, std::string> player_role(Session &s, std::uint64_t sender, bool local, bool marks) {
    if (!sender) return {};
    const bool dedicated = dedicated_host(s);
    if (dedicated && sender == s.host_id) return {nametag_admin, {}}; // the server itself
    const auto *peer = local ? nullptr : find_peer(s, sender);
    const bool vouched = local || (peer && steam_vouched(s, *peer));
    // Who the backend says a player is comes before what they are in this lobby, unless they
    // have turned their tag off (the Special page), which their appearance tells everyone.
    if (marks && vouched && (local ? own_tag_shown() : shows_tag(*peer)))
        if (const auto mark = identity_mark(sender)) return mark_role(*mark);
    if (dedicated && (local ? s.server_admin : peer && peer->member.admin)) return {nametag_admin, "Admin"};
    if (!dedicated && (local ? s.mode == Mode::host : sender == s.host_id)) return {nametag_host, "Host"};
    if (!local && vouched) {
        refresh_friends(s);
        if (std::binary_search(s.friend_ids.begin(), s.friend_ids.end(), sender)) return {nametag_friend, "Friend"};
    }
    return {nametag_white, {}};
}
void add_chat(Session &s, std::uint64_t sender, std::string name, std::string text, bool local, bool marks) {
    if (name.empty()) name = sender ? "Player" : "ReSkate";
    auto [color, tag] = player_role(s, sender, local, marks);
    s.chat.push_back({++s.chat_sequence, sender, now_us(), std::move(name), std::move(text), local, color, std::move(tag)});
    while (s.chat.size() > multiplayer_chat_history) s.chat.pop_front();
    publish_chat(s);
}
} // namespace dingosdk::multiplayer::session_detail
