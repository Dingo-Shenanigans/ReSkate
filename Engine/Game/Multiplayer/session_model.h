#pragma once
#include "distance_settings.h"
#include "object_placement.h"
#include "session_limits.h"
#include "tick_settings.h"
#include "voice_settings.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dingosdk {
constexpr unsigned max_map_rotation = 1440; // minutes a server's rotation keeps one map, at most
// A dedicated server's name: 1 to 64 letters, digits, spaces and - _ [ ] ( ), with a
// letter or digit among them and no space at either end. A server refuses any other
// name, and the server browser does not show one.
inline constexpr char server_name_rule[] = "1 to 64 letters, numbers, spaces and - _ [ ] ( )";
[[nodiscard]] constexpr bool valid_server_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 64 || name.front() == ' ' || name.back() == ' ') return false;
    bool named{};
    for (const auto c : name) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!word && std::string_view(" -_[]()").find(c) == std::string_view::npos) return false;
        named |= word;
    }
    return named;
}
struct MultiplayerLobby {
    std::uint64_t id{}, owner{};
    std::string name, map, code;
    bool password_required{};
    int players = 1, capacity = multiplayer_player_limit;
    // A dedicated server rather than a player's lobby: joined by its code.
    bool dedicated{};
    int ping = -1; // ms, when the server answered directly
};
struct MultiplayerPlayer {
    std::uint64_t id{};
    std::string name;
    bool connected{}, visible{};
    std::string native_status, cosmetic_status, audio_status, ui_status;
    std::string route;
    float pose_hz{};
    unsigned pose_target_tps = 20;
    float distance_m = -1;
    std::uint64_t pose_age_ms{};
    float native_pose_hz{}, native_board_hz{};
    float native_animation_hz{}, native_animation_ms{}, pose_apply_ms{};
    std::uint64_t native_animation_skipped{};
    std::string playback;
    unsigned prediction_ms{};
    bool correcting{};
    std::uint64_t epoch{};
    // The player's party (0 = none), whether they lead it, and whether it is the local player's.
    std::uint32_t party{};
    bool party_leader{}, party_member{};
};
// A party invite waiting for an answer (dedicated servers).
struct MultiplayerPartyInvite {
    std::uint64_t from{};
    std::string name;
};
struct MultiplayerClientTiming {
    float callback_hz{}, gap_max_ms{}, work_ms{}, work_max_ms{};
    // Capture/setup, receive/routing, encode/send, remote playback, diagnostics.
    std::array<float, 5> mean_ms{}, peak_ms{};
};
// The host settings last used, saved in the local profile, so the host pages
// open with them. The password itself is never saved.
struct MultiplayerHostPreferences {
    bool loaded{}, public_lobby{true}, password_required{};
    int capacity = multiplayer_lobby_player_limit;
    unsigned tps = multiplayer_default_tps;
    std::string lobby_name;
};
// Session text chat. A message is UTF-8 without control characters, at most
// this many bytes; the log keeps the most recent lines of the session.
inline constexpr std::size_t multiplayer_chat_max_bytes = 200;
inline constexpr std::size_t multiplayer_chat_history = 50;
struct MultiplayerChatLine {
    std::uint64_t sequence{};   // local arrival order, increasing within the process
    std::uint64_t sender{};     // Steam id; 0 for a notice from ReSkate itself
    std::uint64_t received{};   // local monotonic time (microseconds) the line arrived
    std::string name, text;
    bool local{};               // sent by this player
    // The sender's role, as their nametag shows it: its colour (IM_COL32 layout, 0 = none)
    // and a tag shown in a box before the name ("Dev", "Creator", "Centrix", "Homie", "Admin", "Host", "Friend" or empty).
    std::uint32_t color{};
    std::string tag;
    // With the chat filter on, `text` is masked and this is the line as sent (same length), so
    // the overlay can keep emote names the filter caught; empty when nothing was masked.
    std::string unmasked;
};
// A command typed into chat with a leading "/" (shown as the player types "/").
struct MultiplayerChatCommand {
    std::string name;        // "/vote map"
    std::string usage;       // "/vote map <map>"
    std::string description;
    std::string argument;    // what Tab completes after it: "player", "map", "time" or ""
};
// What the chat overlay reads each frame: cheap to copy, unlike the full model.
struct MultiplayerChat {
    bool available{};           // in a session that can carry chat
    std::uint64_t latest{};     // sequence of the newest line, 0 when empty
    std::vector<MultiplayerChatLine> lines;
    std::vector<MultiplayerChatCommand> commands; // what "/" offers in this session
    std::vector<std::string> players, maps;       // what their arguments complete to
};
// A player this PC's lobbies never admit, kept in the local profile.
struct MultiplayerBan {
    std::uint64_t id{};        // SteamID64
    std::string name;          // as they were known when banned; may be empty
    std::int64_t added{};      // Unix time
};
struct MultiplayerModel {
    VoiceModel voice;
    std::vector<MultiplayerBan> bans;
    unsigned tps = multiplayer_default_tps;
    ObjectPlacement object_placement = ObjectPlacement::everyone;
    // Host setting: whether guests may use noclip / No Bail (the host and server admins always may).
    bool guest_noclip{true}, guest_no_bail{true}, guest_boosts{true};
    // Host setting: guests skate with the host's physics tuning (on a dedicated server: the
    // game's own). Guest: what that does here, while enforced.
    bool enforce_tuning{true};
    std::string tuning_status;
    // Local display preference: the floating name label above each peer.
    bool nametags{true};
    // Nametag style: ReSkate's own (name, distance, role colours, dots) or the game's.
    bool custom_nametags{true};
    // Local: whether session text chat shows at all (and T opens it).
    bool chat_visible{true};
    // Local: bad words in chat names and messages show as **** (on by default).
    bool chat_filter{true};
    // Local: chat bubbles above each skater's head. `chat_bubbles_distance` is how far away
    // a player may be and still show one, `chat_bubbles_duration` how many seconds a line
    // stays before it fades, `chat_bubbles_history` how many recent lines stack up.
    // `chat_bubbles_own` also shows this player's own lines.
    bool chat_bubbles{true};
    bool chat_bubbles_own{};
    float chat_bubbles_distance{40.f};
    float chat_bubbles_duration{5.f};
    int chat_bubbles_history{3};
    // Local: the tag the ReSkate backend gives this player ("Dev", "Creator", "Centrix" or "Homie"; empty
    // for most players) and its role colour, and whether they show it, and the animated items
    // that come with it, to everyone.
    std::string identity_tag;
    std::uint32_t identity_tag_colour{};
    bool identity_tag_shown{true}, identity_items_shown{true};
    // Local: how each of this player's marked cosmetics is coloured, for the Special page: what
    // they wear in each slot ("Top", "Shoes", ...), then the parts of their board. mode: 0 what
    // their list gives (`identity_animation`: "RAINBOW", "RED", "BLUE" or "GOLD"), 1 off, 2 a gradient
    // between the two colours they picked, 3 the first of them alone. speed: 0 normal, 1 slow,
    // 2 fast.
    struct IdentityStyle {
        std::string name;
        int mode{}, speed{};
        std::array<float, 3> from{}, to{};
    };
    std::vector<IdentityStyle> identity_styles;
    std::string identity_animation;
    float voice_range = default_voice_range;  // how far the host (or server) forwards proximity voice
    // In a dedicated server's session: the server is the host but not a player.
    // Admins it lists may change its settings, and its map through Levels.
    bool dedicated{}, server_admin{};
    // `bans` is the dedicated server's list (for its admins) rather than this PC's.
    bool server_bans{};
    unsigned server_ban_total{};
    // For its admins: the levels (assets) the dedicated server can switch to.
    std::vector<std::string> server_maps;
    std::vector<std::string> server_map_pool; // in rotation order; empty: every map
    unsigned server_map_rotation{};           // minutes per map (0: off)
    bool server_map_votes{};                  // players may vote for a map
    MultiplayerHostPreferences saved_host;
    bool force_world_layers{};
    std::uint64_t host_id{};
    std::string local_name;
    std::string lobby_name;
    std::string object_status;
    MultiplayerClientTiming client_timing;
    MultiplayerDistances distances;
    bool password_required{};
    int players = 1, capacity = multiplayer_player_limit;
    std::vector<MultiplayerPlayer> roster;
    // The local player's party. Players form their own, in a lobby as on a dedicated server
    // (`parties`: invite, leave, kick... are available in any session).
    std::uint32_t party{};
    bool party_leader{}, party_open{}, parties{};
    std::vector<MultiplayerPartyInvite> party_invites; // newest last
    bool active{}, hosting{}, connected{}, echo{}, remote_visible{}, local_ready{};
    std::uint64_t local_id{}, peer_id{}, sent{}, received{}, dropped{}, pose_updates{}, board_pose_updates{};
    std::size_t skater_bones{}, board_bones{};
    std::string invite, map;
    std::string status = "Multiplayer is off.";
    std::string native_status;
    std::string cosmetic_status;
    std::string audio_status;
    std::uint64_t audio_captured{}, audio_played{};
    std::string peer_name, player_ui_status;
    std::uint64_t player_map_updates{};
    bool public_host{}, lobby_listed{}, lobby_searching{}, lobby_joining{}, lobby_searched{};
    std::uint64_t public_lobby{}; // Verified public listing, never a private invitation.
    std::string lobby_status, browser_status = "Refresh to find public ReSkate lobbies.";
    std::vector<MultiplayerLobby> lobbies;
    bool network_telemetry{};
    unsigned prioritized_connections{};
    std::uint64_t cosmetic_queue_us{};
    unsigned direct_connections{}, fallback_streams{}, direct_upload_limit{};
    int ping_ms{}, send_rate{}, pending_bytes{};
    float outgoing_bps{}, incoming_bps{}, delivery_local = -1, delivery_remote = -1;
    std::uint64_t queue_us{}, skipped_updates{}, send_failures{}, invalid_messages{}, sent_bytes{},
        raw_sent_bytes{};
};
inline bool multiplayer_controls_level(const MultiplayerModel& model) noexcept {
    // A server admin's level choice is sent to the server, which moves everyone.
    return model.lobby_joining || (model.active && !model.hosting && !model.echo && !model.server_admin);
}
} // namespace dingosdk
